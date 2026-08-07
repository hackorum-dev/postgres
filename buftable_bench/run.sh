#!/usr/bin/env bash
#
# run.sh [size ...]
#
# Build the CURRENTLY CHECKED-OUT BRANCH of this repo (its HEAD) as a release,
# then run the buffer-mapping-table probe and print the per-op numbers for it.
# Single arm (just this branch's buf_table.c) — for the flat-vs-dynahash A/B
# use scripts/compare_probe.sh.
#
# The build is cached per commit under buftable_bench/.builds/<commit>, so the
# first run for a commit takes a few minutes and later runs are instant.
# It exports the committed tree (git archive) into a temp dir to build, so your
# working checkout is never touched.
#
# Env: PG_CONFIG (use this prebuilt install instead of building),
#      BUFTABLE_PROBE_ROUNDS (default 10), BUFTABLE_BENCH_WORK.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODSRC="$HERE/instrumentation/buftable_bench_module"
WORK="${BUFTABLE_BENCH_WORK:-$HERE/_work}"; mkdir -p "$WORK"
ROUNDS="${BUFTABLE_PROBE_ROUNDS:-10}"
SIZES="${*:-1GB}"

repo="$(git -C "$HERE" rev-parse --show-toplevel)"
commit="$(git -C "$repo" rev-parse --short HEAD)"
branch="$(git -C "$repo" rev-parse --abbrev-ref HEAD)"
label="$branch@$commit"

# ---- locate or build a release install of the current HEAD ------------------
if [[ -n "${PG_CONFIG:-}" ]]; then
	BIN="$("$PG_CONFIG" --bindir)"
	echo "==> using PG_CONFIG build: $("$PG_CONFIG" --version) [$BIN]" >&2
else
	prefix="$HERE/.builds/$commit"
	if [[ -x "$prefix/bin/pg_config" ]] && \
	   [[ -e "$("$prefix/bin/pg_config" --pkglibdir)/buftable_bench.so" ]]; then
		echo "==> reusing cached release build of $label  [$prefix]" >&2
	else
		echo "==> building $label as release (first run for this commit, ~2-4 min)..." >&2
		src="$(mktemp -d "$WORK/src.$commit.XXXX")"
		git -C "$repo" archive HEAD | tar -x -C "$src"
		mkdir -p "$src/src/test/modules/buftable_bench"
		cp "$MODSRC"/* "$src/src/test/modules/buftable_bench"/
		(
			cd "$src"
			./configure --prefix="$prefix" --without-icu --without-zlib --without-readline >/dev/null
			make -s -j"$(nproc)" install >/dev/null
			make -s -C src/test/modules/buftable_bench install >/dev/null
		) || { echo "build failed; see $src" >&2; exit 1; }
		rm -rf "$src"
		echo "==> built $label" >&2
	fi
	BIN="$prefix/bin"
fi

# ---- run the probe per size -------------------------------------------------
size_to_bytes() {
	local s="${1^^}"
	case "$s" in
		*GB) echo $(( ${s%GB} * 1024 * 1024 * 1024 ));;
		*MB) echo $(( ${s%MB} * 1024 * 1024 ));;
		*KB) echo $(( ${s%KB} * 1024 ));;
		*)   echo "$s";;
	esac
}

echo
echo "===== buftable_bench: $label (random access, rounds=$ROUNDS) ====="
for SIZE in $SIZES; do
	DATADIR="$(mktemp -d "$WORK/pgdata.${SIZE}.XXXX")"
	SOCKDIR="$(mktemp -d /tmp/pgb.XXXXXX)"
	N="$(awk -v b="$(size_to_bytes "$SIZE")" 'BEGIN{printf "%d", 0.8*b/8192}')"

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
	"$BIN/pg_ctl" -D "$DATADIR" -l "$DATADIR/log" -w start >/dev/null

	echo "-- shared_buffers=$SIZE  (n=$N keys) --"
	"$BIN/psql" -h "$SOCKDIR" -d postgres -q -P pager=off \
		-c "CREATE EXTENSION buftable_bench;" \
		-c "SELECT op, round(avg_ns::numeric,3) AS avg_ns, count
		    FROM buftable_bench_probe($N, $ROUNDS)
		    ORDER BY array_position(ARRAY['insert','lookup_hit','lookup_miss','delete'], op);"

	"$BIN/pg_ctl" -D "$DATADIR" -m immediate stop >/dev/null 2>&1 || true
	rm -rf "$DATADIR" "$SOCKDIR"
done
echo "================================================================"
