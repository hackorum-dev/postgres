# buftable_bench

A micro-benchmark of PostgreSQL's shared **buffer mapping table** ops. Two SQL functions call
`BufTableLookup` / `BufTableInsert` / `BufTableDelete` **directly** on the live shared table and
**bulk-time** each op (one rdtsc pair ÷ N) — no `ReadBuffer`, no page copy, no per-op rdtsc. Both
operate on free buffer slots + synthetic tags and restore the table afterward.

- `buftable_bench_probe(n, rounds, random)` — steady-state, load-factor ~1 (chains ≈ 1 entry):
  insert / lookup_hit / lookup_miss / delete.
- `buftable_bench_collide(chain_len, total_entries, rounds, random, delete_under_lock)` —
  **worst-case hash collisions**: forces `total_entries` tags into `total_entries/chain_len` bucket
  chains of length `chain_len`, then times `lookup_hit_full` (chains intact) and `delete_drain`
  (draining). Motivated by the review concern that the flat table's `BufTableDelete()` runs **while
  the buffer-header spinlock is held** in `InvalidateBuffer()` (the dynahash baseline deleted after
  releasing it), so `delete_drain` ns *is* the extra spinlock-hold the restructuring adds per
  invalidate, and sweeping `chain_len` shows how it scales with collisions. It also reports
  **`lock_hold`** — the *real* average buffer-header spinlock hold time: the probe actually calls
  `LockBufHdr`/`UnlockBufHdrExt` on a spare buffer around a replica of the `InvalidateBuffer` critical
  section, with `BufTableDelete` **inside** the hold when `delete_under_lock=true` (flat) or **after**
  the unlock when `false` (origin/dynahash). `delete_drain` is the flat-*added* component;
  `lock_hold` is the *total* hold in each arm (single-backend → duration, not contention).

The whole thing is one self-contained test-module extension — the **only change vs stock
PostgreSQL**. x86_64 only (uses `rdtsc`).

## Files

```
buftable_bench/
├── run.sh                                   # build the current branch (release) + run + print numbers
├── instrumentation/buftable_bench_module/   # the extension (drop into src/test/modules/)
│   ├── buftable_bench.c                      #   buftable_bench_probe() + buftable_bench_collide()
│   ├── buftable_bench--1.0.sql               #   CREATE FUNCTION for both
│   ├── buftable_bench.control
│   ├── Makefile
│   └── meson.build
├── scripts/
│   ├── bench_probe.sh     <prefix> <size>    # run the probe against one explicit install
│   ├── compare_probe.sh   [sizes...]         # flat-vs-dynahash A/B (needs two builds)
│   ├── bench_collide.sh   <prefix> <size>    # run the collision sweep against one install
│   └── compare_collide.sh [sizes...]         # flat-vs-dynahash collision A/B (needs two builds)
└── results/
    ├── probe_summary.txt                     # recorded probe A/B numbers
    ├── compare_probe.results.txt
    └── collide_summary.txt                   # recorded collision A/B numbers + conclusion
```
(`.builds/` and `_work/` are local build/scratch caches — gitignored.)

## How to run

**The current branch** (builds this repo's `HEAD` as a release, caches it per commit, runs, and
prints the per-op numbers):

```sh
buftable_bench/run.sh                 # default shared_buffers = 1GB
buftable_bench/run.sh 256MB 4GB 16GB  # any sizes
```
First run for a commit builds (~30 s–few min); later runs are instant. Output (to stdout), labeled
`branch@commit`:
```
-- shared_buffers=1GB  (n=104857 keys) --
     op      | avg_ns |  count
 insert      | 18.555 | 1048570
 lookup_hit  | 23.064 | 1048570
 lookup_miss | 21.902 | 1048570
 delete      | 24.569 | 1048570
```
Knobs: `BUFTABLE_PROBE_ROUNDS` (default 10); `PG_CONFIG=/path/bin/pg_config` to skip the build and
use an existing install. (`run.sh` is **single-arm** — it measures the current branch only.)

**Flat-vs-dynahash A/B** (optional; needs two installs at `~/pg-bench/{flat,dyna}`):
```sh
buftable_bench/scripts/compare_probe.sh 256MB 4GB 16GB
```

**Worst-case collision A/B** (the `InvalidateBuffer` spinlock-hold question; needs the same two
installs, with the module rebuilt into each — `make -C src/test/modules/buftable_bench install`):
```sh
buftable_bench/scripts/compare_collide.sh 256MB 4GB          # sweeps chain length G=1..64
BUFTABLE_COLLIDE_ROUNDS=40 buftable_bench/scripts/compare_collide.sh 256MB
```
Knobs: `BUFTABLE_COLLIDE_CHAINS` (default `1 2 4 8 16 32 64`), `BUFTABLE_COLLIDE_ROUNDS` (20),
`BUFTABLE_COLLIDE_TOTAL` (0 = auto). `delete_drain` ns is the extra header-spinlock hold the flat
table adds; `lookup_hit_full` (must rise ~linearly in G) is the collision sanity check. Recorded
numbers + conclusion in `results/collide_summary.txt`. **Note:** insert and delete phases use
*independent* random orders on purpose — flat head-inserts while dynahash tail-appends, so a shared
order would unfairly pin flat to the tail (worst) and dynahash to the head (best).
