#!/usr/bin/env bash
#
# compare_probe.sh [size ...]
#
# Pollution-free in-place A/B of the shared buffer mapping table for all three
# ops (insert / lookup_hit / lookup_miss / delete), via buftable_bench_probe().
# Prints, per size, dynahash vs flat avg_ns and speedup = dynahash/flat (>1 =
# flat faster).  Sizes default "256MB 4GB 16GB" or $BUFTABLE_CAPI_SIZES or args.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/bench_probe.sh"
FLAT=/home/dhruv.aron/pg-bench/flat
DYNA=/home/dhruv.aron/pg-bench/dyna
SCRATCH="${BUFTABLE_BENCH_WORK:-$HERE/_work}"; mkdir -p "$SCRATCH"
RESULTS="$SCRATCH/compare_probe.results.txt"; : > "$RESULTS"

if [[ $# -gt 0 ]]; then SIZES="$*"; else SIZES="${BUFTABLE_CAPI_SIZES:-256MB 4GB 16GB}"; fi

for size in $SIZES; do
	for prefix in "$DYNA" "$FLAT"; do
		echo ">>> $(basename "$prefix") @ $size" >&2
		bash "$BENCH" "$prefix" "$size" | grep '^RESULT ' >> "$RESULTS"
	done
done

echo
echo "===== pollution-free direct-probe A/B (real shared table) ====="
awk '
{ arm=$2; size=$3; op=$4; v[size,op,arm]=$5; cnt[size,op,arm]=$6;
  if(!(size in seen)){seen[size]=1; order[++ni]=size} }
END{
  split("insert lookup_hit lookup_miss delete", ops, " ");
  for(i=1;i<=ni;i++){ s=order[i];
    printf "\n=== shared_buffers = %s  (samples/op %s) ===\n", s, cnt[s,"insert","flat"];
    printf "%-12s %12s %12s %14s\n","op","dynahash ns","flat ns","speedup(dh/ft)";
    for(j=1;j<=4;j++){ o=ops[j];
      dh=v[s,o,"dyna"]; ft=v[s,o,"flat"];
      if(dh==""||ft==""){ printf "%-12s %12s %12s %14s\n",o,(dh==""?"-":dh),(ft==""?"-":ft),"n/a"; continue }
      printf "%-12s %12.3f %12.3f %13.2fx\n", o, dh, ft, (ft>0?dh/ft:0);
    }
  }
}' "$RESULTS"
echo "==============================================================="
