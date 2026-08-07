#!/usr/bin/env bash
#
# bench_collide.sh <pg_install_prefix> <shared_buffers_size> [delete_under_lock]
#
# WORST-CASE hash-collision benchmark of the shared buffer mapping table, via
# buftable_bench_collide() — which forces many tags into ONE bucket chain and
# bulk-times BufTableLookup (chains intact) and BufTableDelete (draining the
# chains).  delete_drain is exactly the work the flat table now does while the
# buffer-header spinlock is held in InvalidateBuffer(); the dynahash baseline
# did its delete AFTER releasing that spinlock, so its hold contribution is 0.
# It also reports lock_hold: the real average buffer-header spinlock hold across
# a replica of the InvalidateBuffer critical section, with BufTableDelete inside
# the hold when delete_under_lock=true (flat) or after it when false (origin).
#
# The optional 3rd arg delete_under_lock (default "true") selects that placement:
# pass "true" for the flat arm, "false" for the dynahash/origin arm.
#
# Sweeps chain length G and emits, per G and op:
#   "RESULT <arm> <size> <G> <op> <avg_ns> <count>"
# ops: insert_build, lookup_hit_full, delete_drain, worst_delete_est, lock_hold.
#
# Env:
#   BUFTABLE_COLLIDE_CHAINS   chain lengths to sweep (default "1 2 4 8 16 32 64")
#   BUFTABLE_COLLIDE_ROUNDS   timed rounds per point   (default 20)
#   BUFTABLE_COLLIDE_TOTAL    total entries E; 0 = module auto (default 0)
set -euo pipefail

PREFIX="${1:?usage: bench_collide.sh <prefix> <size> [delete_under_lock]}"
SIZE="${2:?usage: bench_collide.sh <prefix> <size> [delete_under_lock]}"
DUL="${3:-true}"
CHAINS="${BUFTABLE_COLLIDE_CHAINS:-1 2 4 8 16 32 64}"
ROUNDS="${BUFTABLE_COLLIDE_ROUNDS:-20}"
TOTAL="${BUFTABLE_COLLIDE_TOTAL:-0}"
ARM="$(basename "$PREFIX")"
BIN="$PREFIX/bin"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH="${BUFTABLE_BENCH_WORK:-$HERE/_work}"; mkdir -p "$SCRATCH"
DATADIR="$(mktemp -d "$SCRATCH/pgdata.${ARM}.${SIZE}.co.XXXX")"
SOCKDIR="$(mktemp -d /tmp/pgb.XXXXXX)"
LOG="$DATADIR/server.log"

log() { echo "[$ARM $SIZE collide] $*" >&2; }
cleanup() { "$BIN/pg_ctl" -D "$DATADIR" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$DATADIR" "$SOCKDIR"; }
trap cleanup EXIT

"$BIN/initdb" -D "$DATADIR" --no-sync -A trust >/dev/null 2>&1
cat >> "$DATADIR/postgresql.conf" <<CONF
shared_buffers = '$SIZE'
jit = off
autovacuum = off
fsync = off
bgwriter_lru_maxpages = 0
listen_addresses = ''
unix_socket_directories = '$SOCKDIR'
CONF

log "start (shared_buffers=$SIZE, chains='$CHAINS', rounds=$ROUNDS, total=$TOTAL, delete_under_lock=$DUL)"
"$BIN/pg_ctl" -D "$DATADIR" -l "$LOG" -w start >/dev/null

"$BIN/psql" -h "$SOCKDIR" -d postgres -q -X -v ON_ERROR_STOP=1 \
	-c "CREATE EXTENSION buftable_bench;" >/dev/null

for G in $CHAINS; do
	OUT="$("$BIN/psql" -h "$SOCKDIR" -d postgres -q -X -At -F' ' -v ON_ERROR_STOP=1 \
		-c "SELECT 'R', op, round(avg_ns::numeric,3), count FROM buftable_bench_collide($G, $TOTAL, $ROUNDS, true, $DUL);" 2>&1)" || {
		log "psql failed at G=$G:"; echo "$OUT" >&2; exit 1;
	}
	echo "$OUT" | awk -v arm="$ARM" -v sz="$SIZE" -v g="$G" \
		'$1=="R"{printf "RESULT %s %s %s %s %s %s\n", arm, sz, g, $2, $3, $4}'
done
log "done"
