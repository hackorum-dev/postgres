#!/usr/bin/env bash
#
# compare_collide.sh [size ...]
#
# Worst-case hash-collision A/B of the shared buffer mapping table (dynahash vs
# flat), via buftable_bench_collide().  For each size, sweeps chain length G and
# prints, per op, dynahash-ns vs flat-ns and speedup = dynahash/flat (>1 = flat
# faster).
#
# The headline is delete_drain: in the flat table BufTableDelete() runs while the
# buffer-header spinlock is held in InvalidateBuffer(), so flat's delete_drain ns
# IS the extra spinlock-hold the restructuring adds.  Dynahash deleted AFTER
# releasing the spinlock, so its contribution to the hold is 0 (its delete_drain
# ns is shown only as the raw table-op cost, for reference).  lookup_hit_full is
# the collision sanity check: ns must rise ~linearly in G in BOTH arms.
#
# Sizes default "256MB 4GB" or $BUFTABLE_COLLIDE_SIZES or args.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH="$HERE/bench_collide.sh"
FLAT=/home/dhruv.aron/pg-bench/flat
DYNA=/home/dhruv.aron/pg-bench/dyna
SCRATCH="${BUFTABLE_BENCH_WORK:-$HERE/_work}"; mkdir -p "$SCRATCH"
RESULTS="$SCRATCH/compare_collide.results.txt"; : > "$RESULTS"

if [[ $# -gt 0 ]]; then SIZES="$*"; else SIZES="${BUFTABLE_COLLIDE_SIZES:-256MB 4GB}"; fi

for size in $SIZES; do
	# Each arm replicates ITS OWN InvalidateBuffer: dynahash deletes AFTER the
	# spinlock unlock (delete_under_lock=false), flat deletes UNDER it (true).
	echo ">>> dyna @ $size (delete_under_lock=false)" >&2
	bash "$BENCH" "$DYNA" "$size" false | grep '^RESULT ' >> "$RESULTS"
	echo ">>> flat @ $size (delete_under_lock=true)" >&2
	bash "$BENCH" "$FLAT" "$size" true | grep '^RESULT ' >> "$RESULTS"
done

echo
echo "===== worst-case collision A/B (real shared table, direct probe) ====="
awk '
{ arm=$2; size=$3; g=$4+0; op=$5; v[size,g,op,arm]=$6;
  if(!(size in seen_s)){seen_s[size]=1; sorder[++ns]=size}
  key=size SUBSEP g; if(!(key in seen_g)){seen_g[key]=1; gcount[size]++; glist[size,gcount[size]]=g} }
END{
  for(si=1; si<=ns; si++){ s=sorder[si];
    # sort this size s chain lengths ascending (simple insertion sort)
    n=gcount[s];
    for(a=1;a<=n;a++) arr[a]=glist[s,a];
    for(a=2;a<=n;a++){ x=arr[a]; b=a-1; while(b>=1 && arr[b]>x){arr[b+1]=arr[b];b--}; arr[b+1]=x }

    printf "\n############ shared_buffers = %s ############\n", s;

    printf "\n--- delete_drain  (flat ns == EXTRA buffer-header spinlock hold; dynahash hold contribution = 0) ---\n";
    printf "%6s %14s %12s %14s\n","G","dynahash ns","flat ns","speedup(dh/ft)";
    for(a=1;a<=n;a++){ g=arr[a];
      dh=v[s,g,"delete_drain","dyna"]; ft=v[s,g,"delete_drain","flat"];
      if(dh==""||ft==""){ printf "%6d %14s %12s %14s\n",g,(dh==""?"-":dh),(ft==""?"-":ft),"n/a"; continue }
      printf "%6d %14.3f %12.3f %13.2fx\n", g, dh, ft, (ft>0?dh/ft:0);
    }

    printf "\n--- lock_hold  (REAL avg buffer-header spinlock hold across the InvalidateBuffer crit-section) ---\n";
    printf "%6s %14s %12s %12s %18s\n","G","dynahash ns","flat ns","flat-dyna","(cf delete_drain ft)";
    for(a=1;a<=n;a++){ g=arr[a];
      dh=v[s,g,"lock_hold","dyna"]; ft=v[s,g,"lock_hold","flat"]; dd=v[s,g,"delete_drain","flat"];
      if(dh==""||ft==""){ printf "%6d %14s %12s %12s %18s\n",g,(dh==""?"-":dh),(ft==""?"-":ft),"n/a","n/a"; continue }
      printf "%6d %14.3f %12.3f %+12.3f %18s\n", g, dh, ft, (ft-dh), (dd==""?"-":sprintf("%.3f",dd));
    }

    printf "\n--- worst_delete_est  (derived: cost of deleting the tail-most entry, ~ per-cmp ns * G) ---\n";
    printf "%6s %14s %12s %14s\n","G","dynahash ns","flat ns","speedup(dh/ft)";
    for(a=1;a<=n;a++){ g=arr[a];
      dh=v[s,g,"worst_delete_est","dyna"]; ft=v[s,g,"worst_delete_est","flat"];
      if(dh==""||ft==""){ printf "%6d %14s %12s %14s\n",g,(dh==""?"-":dh),(ft==""?"-":ft),"n/a"; continue }
      printf "%6d %14.3f %12.3f %13.2fx\n", g, dh, ft, (ft>0?dh/ft:0);
    }

    printf "\n--- lookup_hit_full  (collision sanity: ns must rise ~linearly in G in BOTH arms) ---\n";
    printf "%6s %14s %12s %14s\n","G","dynahash ns","flat ns","speedup(dh/ft)";
    for(a=1;a<=n;a++){ g=arr[a];
      dh=v[s,g,"lookup_hit_full","dyna"]; ft=v[s,g,"lookup_hit_full","flat"];
      if(dh==""||ft==""){ printf "%6d %14s %12s %14s\n",g,(dh==""?"-":dh),(ft==""?"-":ft),"n/a"; continue }
      printf "%6d %14.3f %12.3f %13.2fx\n", g, dh, ft, (ft>0?dh/ft:0);
    }
  }
}' "$RESULTS"
echo
echo "======================================================================"
echo "Read: lock_hold is the REAL average buffer-header spinlock hold time of the"
echo "InvalidateBuffer() critical section in each arm (dynahash deletes after the"
echo "unlock, flat under it).  flat-dyna is the extra hold the restructuring adds"
echo "and should track flat delete_drain (the moved BufTableDelete is the only"
echo "difference).  Single-backend: hold DURATION, not lock contention."
