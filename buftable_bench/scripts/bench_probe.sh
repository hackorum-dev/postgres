#!/usr/bin/env bash
#
# bench_probe.sh <pg_install_prefix> <shared_buffers_size>
#
# Pollution-free, in-place benchmark of the REAL shared buffer mapping table for
# lookup (hit+miss), insert, and delete, via buftable_bench_probe() — which calls
# BufTable{Insert,Lookup,Delete} directly (no ReadBuffer, no 8 KB page copy, no
# per-op rdtsc; each op's loop is bulk-timed).  No table/prewarm needed: it uses
# free buffer slots + synthetic tags and restores the table afterward.
#
# Emits "RESULT <arm> <size> <op> <avg_ns> <count>".
# Env: BUFTABLE_PROBE_ROUNDS (default 10).
set -euo pipefail

PREFIX="${1:?usage: bench_probe.sh <prefix> <size>}"
SIZE="${2:?usage: bench_probe.sh <prefix> <size>}"
ROUNDS="${BUFTABLE_PROBE_ROUNDS:-10}"
ARM="$(basename "$PREFIX")"
BIN="$PREFIX/bin"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH="${BUFTABLE_BENCH_WORK:-$HERE/_work}"; mkdir -p "$SCRATCH"
DATADIR="$(mktemp -d "$SCRATCH/pgdata.${ARM}.${SIZE}.pr.XXXX")"
SOCKDIR="$(mktemp -d /tmp/pgb.XXXXXX)"
LOG="$DATADIR/server.log"

log() { echo "[$ARM $SIZE probe] $*" >&2; }
cleanup() { "$BIN/pg_ctl" -D "$DATADIR" -m immediate stop >/dev/null 2>&1 || true; rm -rf "$DATADIR" "$SOCKDIR"; }
trap cleanup EXIT

size_to_bytes() {
	local s="${1^^}"
	case "$s" in
		*GB) echo $(( ${s%GB} * 1024 * 1024 * 1024 ));;
		*MB) echo $(( ${s%MB} * 1024 * 1024 ));;
		*)   echo "$s";;
	esac
}
SB="$(size_to_bytes "$SIZE")"
N="$(awk -v b="$SB" 'BEGIN{printf "%d", 0.8*b/8192}')"   # ~0.8x NBuffers -> load factor ~1

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

log "start (shared_buffers=$SIZE, n=$N, rounds=$ROUNDS)"
"$BIN/pg_ctl" -D "$DATADIR" -l "$LOG" -w start >/dev/null

OUT="$("$BIN/psql" -h "$SOCKDIR" -d postgres -q -X -At -F' ' -v ON_ERROR_STOP=1 \
	-c "CREATE EXTENSION buftable_bench;" \
	-c "SELECT 'R', op, round(avg_ns::numeric,3), count FROM buftable_bench_probe($N, $ROUNDS);" 2>&1)" || {
	log "psql failed:"; echo "$OUT" >&2; exit 1;
}

echo "$OUT" | awk -v arm="$ARM" -v sz="$SIZE" '$1=="R"{printf "RESULT %s %s %s %s %s\n", arm, sz, $2, $3, $4}'
log "done"
