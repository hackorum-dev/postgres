# buftable_bench

A micro-benchmark of PostgreSQL's shared **buffer mapping table** ops. The SQL function
`buftable_bench_probe(n, rounds, random)` calls `BufTableLookup` / `BufTableInsert` /
`BufTableDelete` **directly** on the live shared table and **bulk-times** each op (one rdtsc pair ÷
N) — no `ReadBuffer`, no page copy, no per-op rdtsc. It operates on free buffer slots + synthetic
tags and restores the table afterward.

The whole thing is one self-contained test-module extension — the **only change vs stock
PostgreSQL**. x86_64 only (uses `rdtsc`).

## Files

```
buftable_bench/
├── run.sh                                   # build the current branch (release) + run + print numbers
├── instrumentation/buftable_bench_module/   # the extension (drop into src/test/modules/)
│   ├── buftable_bench.c                      #   buftable_bench_probe() — the probe
│   ├── buftable_bench--1.0.sql               #   CREATE FUNCTION buftable_bench_probe(...)
│   ├── buftable_bench.control
│   ├── Makefile
│   └── meson.build
├── scripts/
│   ├── bench_probe.sh   <prefix> <size>      # run the probe against one explicit install
│   └── compare_probe.sh [sizes...]           # flat-vs-dynahash A/B (needs two builds)
└── results/
    ├── probe_summary.txt                     # recorded A/B numbers
    └── compare_probe.results.txt
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
