# MDBX Benchmark Suite

A self-contained C++20 benchmark suite for [libmdbx](https://github.com/erthink/libmdbx) that measures bulk-load throughput and point-read latency (hot & cold cache).

The suite supports **two storage layouts**:
- `single` (default): one MDBX env with multiple DBIs inside it.
- `per_dbi`: one MDBX env *per DBI* (separate directory, file, and lock per DBI).

Both layouts run the same phases against the same schema, so the CSVs from both can be compared directly. The suite is reproducible (MDBX is pinned via CMake `FetchContent`), configurable via CLI flags, and emits one structured CSV per phase so a matrix of runs can be analyzed together.

## Contents

- [Requirements](#requirements)
- [Building](#building)
- [Quick start](#quick-start)
- [Storage layouts](#storage-layouts)
- [CLI reference](#cli-reference)
- [Phases](#phases)
- [Output files & columns](#output-files--columns)
- [Environment-flag tradeoffs](#environment-flag-tradeoffs)
- [Methodology notes](#methodology-notes)
- [Troubleshooting](#troubleshooting)

---

## Requirements

| Component | Minimum | Notes |
|---|---|---|
| Compiler | C++20 (GCC 11+, Clang 14+) | |
| CMake | 3.20+ | `FetchContent_MakeAvailable` |
| Git | any | needed by `FetchContent` to clone libmdbx |
| OS | Linux | cold-read phase writes `/proc/sys/vm/drop_caches`, which is Linux-only |
| Disk | ~20 GB free | The default schema loads ~40 M total records; map size is reserved at 128 GB upper-bound |
| RAM | 16 GB recommended | Hot-read measurements assume the working set fits in RAM |

MDBX itself has no external runtime dependencies. The build links it statically.

## Building

```bash
# from the repo root
./scripts/build.sh
# or manually:
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j"$(nproc)"
# binary: ./build/mdbx_bench
```

The first configure step clones libmdbx at the pinned tag (`v0.13.12`) into `build/_deps/libmdbx-src`. Subsequent configures reuse the cache.

To upgrade MDBX, edit `GIT_TAG` in [CMakeLists.txt](CMakeLists.txt) and delete `build/_deps/libmdbx-*`.

## Quick start

```bash
# 1. Build
./scripts/build.sh

# 2. Load both layouts (writes — this is the slow step)
./scripts/load_single.sh        # populates ./bench_db_single
./scripts/load_per_dbi.sh       # populates ./bench_db_per_dbi

# 3. Run read benchmarks (sweep threads x cross_dbi x reps across both layouts)
./scripts/read_hot.sh           # no root needed
sudo -E ./scripts/read_cold.sh  # cold phase needs root for drop_caches

# 4. Inspect output
ls results/
#   load.csv  hot_reads.csv  cold_reads.csv
column -ts, < results/hot_reads.csv | less -S
```

Run `./build/mdbx_bench --help` for the full flag list.

### Run an individual phase

Each phase is selected with `--phase`:

```bash
# Load (writes)
./build/mdbx_bench --phase load --db-path ./bench_db_single --layout single

# Hot reads (mdbx_env_warmup before each config)
./build/mdbx_bench --phase hot_reads  --db-path ./bench_db_single --layout single \
                   --threads 1,4,8,16 --cross-dbi both --reps 3

# Cold reads (drops OS page cache before each config — root required)
sudo ./build/mdbx_bench --phase cold_reads --db-path ./bench_db_single --layout single \
                       --threads 1,4,8,16 --cross-dbi both --reps 3
```

The read phases sweep `(threads × cross_dbi × reps)` internally and emit one CSV row per repetition. They open and close the env(s) once per config (cold path includes env open intentionally).

## Storage layouts

The `--layout` flag selects how DBIs are arranged on disk:

| Layout | Disk shape | Atomicity | Concurrency | Use case |
|---|---|---|---|---|
| `single` (default) | `<db-path>/mdbx.dat` + `mdbx.lck` with N DBIs inside one env | A single write txn can touch all DBIs atomically | One writer for the whole env; readers serialize on env's reader slots | Most production setups |
| `per_dbi` | `<db-path>/<dbi_name>/mdbx.dat` + `mdbx.lck` per DBI; N independent envs | No cross-DBI atomicity — each env commits independently | Each env has its own writer slot, so DBIs can be written in parallel | Workloads where DBIs are functionally independent and you want to remove the global write lock |

In `per_dbi` mode the `--map-size-gb` budget is split evenly across the N envs (each env gets `map_size / DBI_COUNT`). Per-DBI overrides are supported via `--dbi-map-size-gb <name:gb>` (repeatable); the default loader scripts pin `vectors:128` because it dwarfs the other DBIs.

### Per-DBI mode notes for reads

- The hot/cold read phases open one read txn per env *per thread* (lazily — only for envs that thread actually touches), so when `cross_dbi=1`, the snapshot is **not globally consistent across DBIs**. For point-read latency this doesn't matter.
- `mdbx_env_warmup` is called on every env before hot reads.
- `drop_caches` flushes the whole OS page cache before cold reads, which affects every env on the host.

## CLI reference

```
Usage: mdbx_bench [options]

Phases:
  --phase load              Bulk-load DBIs (writes). Default.
  --phase cold_reads        Drops VM caches before each config; appends to cold_reads.csv.
                            REQUIRES root (or write access to /proc/sys/vm/drop_caches).
  --phase hot_reads         Calls mdbx_env_warmup before each config; appends to hot_reads.csv.

Common options:
  --db-path <path>          Default: ./bench_db
  --out-dir <path>          Directory for per-phase CSV files. Default: ./results
  --run-id <string>         Tag written into the run_id column of every row.
                            Default: auto-generated from sync-mode + writemap + phase + timestamp.
  --map-size-gb <n>         Default: 128
  --dbi-map-size-gb <name:n>  Per-DBI map size override for per_dbi layout. Repeatable.
                            E.g. --dbi-map-size-gb vectors:128
  --writemap <0|1>          Default: 1   (MDBX_WRITEMAP)
  --nordahead <0|1>         Default: 1   (MDBX_NORDAHEAD)
  --liforeclaim <0|1>       Default: 1   (MDBX_LIFORECLAIM)
  --sync-mode <s>           default | safe_nosync | utterly_nosync   Default: safe_nosync
  --layout <s>              single | per_dbi   Default: single

Load-phase options:
  --batch-size <n>          Writer batch size override
  --dump-histograms         Also emit load_hist.csv with one row per histogram bucket

Read-phase options (cold_reads / hot_reads):
  --threads <list>          Comma-separated thread counts. Default: 1,4,8,16
  --cross-dbi <true|false|both>   Default: both
  --reads-per-thread <n>    Default: 100000
  --reps <n>                Repetitions per config. Default: 3
  --seed <u64>              Base RNG seed (per-thread seed = base + tid). Default: 0xC0FFEE
  --help
```

If you re-run with the same `--out-dir` but a different `--run-id`, rows are **appended** to the existing CSV files; the header is written only when the file is first created. Each invocation also prepends a `# === <run_id> ===` divider line before its first data row, so back-to-back runs are easy to slice apart visually (pandas: `pd.read_csv(..., comment='#')`).

## Phases

| Phase | What it measures | Why it matters |
|---|---|---|
| `load` | Bulk-insert throughput and commit p50/p99 across all DBIs at a fixed batch size | Establishes write-side baseline; produces the data that the read phases consume |
| `hot_reads` | Point-read latency with cache prewarmed via `mdbx_env_warmup(MDBX_warmup_force \| MDBX_warmup_oomsafe)`, swept over `threads × cross_dbi × reps` | The latency you see in production once the working set is resident |
| `cold_reads` | Same sweep as `hot_reads` but with `/proc/sys/vm/drop_caches=3` *and* a fresh env open before each config | Isolates disk-read cost. The hot vs cold gap is your effective cache benefit |

### Read-phase mechanics

- One `MDBX_TXN_RDONLY` transaction per thread per env (lazily opened; aborted at end).
- Each thread pre-generates its N random keys *before* timing starts, seeded with `base_seed + tid`. This makes `single` vs `per_dbi` see identical key sequences for direct comparison, and runs are bit-for-bit reproducible across reps that share a seed.
- `cross_dbi=false`: thread `tid` is pinned to DBI `tid % DBI_COUNT`; all N keys come from that DBI's keyspace.
- `cross_dbi=true`: each key is drawn from a uniformly random DBI.
- Per-op latency is recorded into a pre-sized `std::vector<uint64_t>` (no `push_back` in the timed loop). After join, latencies are merged, sorted, and exact p50/p95/p99/mean/min/max are computed.
- `wall_time_ms` is `max(thread_end) - min(thread_start)` — the elapsed window during which any thread was reading. `throughput_rps = total_reads / wall_time_ms`.
- A `checksum` column XOR-folds every returned value byte across all threads; it deters dead-code elimination and confirms reproducibility (identical config + seed → identical checksum, including across `single`/`per_dbi`).

### Default schema

The default schema ([src/schema.hpp](src/schema.hpp)) is seven DBIs:

| DBI | key size | val size | records | flags |
|---|---|---|---|---|
| `vectors` | 4 B | 1540 B | 10 000 000 | `MDBX_INTEGERKEY` |
| `meta` | 4 B | 75 B | 10 000 000 | — |
| `id` | 7 B | 4 B | 10 000 000 | — |
| `filter_schema` | 17 B | 48 B | 5 | — |
| `filter_numeric_inverted` | 7 B | 10 000 B | 14 000 | — |
| `filter_numeric_forward` | 10 B | 4 B | 10 000 000 | — |
| `filter_category` | 16 B | 2 B | 10 | — |

Keys are dense: `seq = 0..record_count-1`. The read benchmark draws keys as `uniform_int(0, record_count-1)` and reconstructs the encoded key with `make_key_int` / `make_key_seq`, so every read is guaranteed to hit. Edit `SCHEMA[]` to change sizes.

## Output files & columns

Every row in every file starts with the same common columns:

```
run_id, timestamp, sync_mode, writemap, nordahead, liforeclaim, layout
```

`layout` is `single` or `per_dbi`. Phase-specific columns follow.

### `load.csv` — one row per (DBI, batch variant)
```
dbi, batch_size, use_append, total_records,
elapsed_ms, records_per_sec, db_size_mb,
commit_p50_us, commit_p99_us
```
- `elapsed_ms` is wall time for the whole DBI load (all batches).
- `commit_p50/p99_us` are over all commits in that load (one per batch).
- `db_size_mb` is measured after the DBI finishes loading.

### `hot_reads.csv` / `cold_reads.csv` — one row per (threads, cross_dbi, rep)
```
threads, cross_dbi, reads_per_thread, rep,
total_reads, wall_time_ms, throughput_rps,
latency_p50_ns, latency_p95_ns, latency_p99_ns,
latency_mean_ns, latency_min_ns, latency_max_ns,
checksum
```
- `total_reads = reads_per_thread × threads`.
- All latency columns are in **nanoseconds**.
- `cross_dbi` is `0` or `1`.
- One row per repetition — no aggregation is done in the benchmark; let your analysis script compute medians/means across reps.

### `load_hist.csv` (when `--dump-histograms` is passed)
Raw histogram dump alongside the summary file:
```
variant, bucket_lo_ns, bucket_hi_ns, count
```
Buckets are power-of-two in nanoseconds; only non-empty buckets are emitted.

### Run dividers

Each CSV invocation emits one comment line before its first data row:

```
run_id,timestamp,...,checksum
# === runA ===
runA,2026-...,...
# === runB ===
runB,2026-...,...
```

`pd.read_csv(path, comment='#')` skips them; `grep -v '^#'` does the same.

## Environment-flag tradeoffs

| Flag | Effect | Tradeoff |
|---|---|---|
| `--writemap 1` (`MDBX_WRITEMAP`) | Writer mmaps the DB writable; eliminates an extra copy on commit | Lower commit latency. But any process bug can corrupt the file via stray writes; not safe with untrusted code in the same address space |
| `--writemap 0` | Writer uses `write(2)` against the file | Slower commits, safer against memory-corruption bugs |
| `--nordahead 1` (`MDBX_NORDAHEAD`) | Disables OS read-ahead | Better for random-access workloads; avoids wasted I/O. Worse for scans |
| `--liforeclaim 1` (`MDBX_LIFORECLAIM`) | Reclaim free pages LIFO (newest first) | Better cache locality, fewer dirty pages written back. Slightly more fragmentation |
| `--sync-mode default` | `MDBX_SYNC_DURABLE` — fsync on every commit | Strongest durability; lowest throughput |
| `--sync-mode safe_nosync` | `MDBX_SAFE_NOSYNC` — fsync deferred, but DB is always crash-consistent | Big throughput win, recent commits may be lost on crash but DB is never corrupted |
| `--sync-mode utterly_nosync` | `MDBX_UTTERLY_NOSYNC` — no fsync at all | Highest throughput, **DB can be corrupted on crash**. Useful for measuring throughput ceilings only |

Sync mode and writemap affect the `load` phase. Read phases open the env with `MDBX_RDONLY | MDBX_NOSTICKYTHREADS` and are unaffected by these durability flags; they are still recorded into the common columns for traceability.

## Methodology notes

- **Timer**: `std::chrono::steady_clock` (monotonic).
- **Read latencies**: exact percentiles from the sorted merged per-op vector (no histogram bucketing) — values are accurate to the nanosecond.
- **Load commit latencies**: power-of-2 nanosecond buckets, 64 buckets total. Percentiles return the upper edge of the containing bucket, which is conservative.
- **Cold reads**: `close_env` → `sync()` → write `3` to `/proc/sys/vm/drop_caches` → `sleep(1)` → `open_env_readonly` → run. If `drop_caches` is not writable (i.e. not root), the program **exits with a clear error** rather than silently producing hot-cache numbers under the cold label.
- **Hot reads**: `open_env_readonly` → `mdbx_env_warmup(force | oomsafe)` on every env → run. (libmdbx 0.13.12 does not expose `MDBX_warmup_touch`; `force` is the equivalent "load pages into RAM" call.)
- **Reader slots**: `max_readers` is set to 256 to provide headroom for `per_dbi × 16 threads`. Each thread holds one slot per env it touches.
- **No mocking of timing**: every `mdbx_get` rc is checked; failures throw with `mdbx_strerror(rc)` and the operation name.
- **Reproducibility**: per-thread RNG seed is `--seed + tid`. Identical `(seed, threads, cross_dbi, reads_per_thread)` produce identical checksums regardless of layout — useful for verifying both layouts saw the same key stream.

## Troubleshooting

### `MDBX_MAP_FULL` during load
Increase `--map-size-gb`. The default is 128 GB of address-space reservation (not disk). In `per_dbi` mode, use `--dbi-map-size-gb <name>:<gb>` to give the large DBI (e.g. `vectors`) a bigger share than the even split.

### `MDBX_READERS_FULL`
The benchmark bumps `max_readers` to 256, which is sufficient for `per_dbi × 16 threads`. If you go higher, edit `make_env_cfg` in [src/main.cpp](src/main.cpp).

### `FATAL: cannot open /proc/sys/vm/drop_caches for write` during cold reads
The cold phase requires root because it writes to `/proc/sys/vm/drop_caches`. Run via `sudo -E ./scripts/read_cold.sh` or `sudo ./build/mdbx_bench --phase cold_reads ...`.

### Hot reads and cold reads have similar p99
Either the working set fits in L3/RAM or the cache drop didn't take. Verify with `free -h` before and after `echo 3 > /proc/sys/vm/drop_caches`. Also confirm `--nordahead 1` was set when loading — without it, the kernel will aggressively prefetch and the cold/hot distinction blurs.

### Read benchmark fails with "path does not exist"
The read phases use `open_env_readonly`, which requires `--db-path` to point at an already-populated database. Run `./scripts/load_single.sh` / `./scripts/load_per_dbi.sh` first.

### `MDBX_APPEND` errors during load
The loader catches the error from `cur.put(..., MDBX_APPEND)` and falls back to `MDBX_UPSERT`. If you see many fallbacks, your key generator is producing out-of-order keys.

### Build can't fetch libmdbx
The repo URL pinned in `CMakeLists.txt` is the GitHub mirror. Upstream development has moved to gitflic.ru; the GitHub mirror still serves tagged releases. If GitHub is unreachable, point `GIT_REPOSITORY` at `https://gitflic.ru/project/erthink/libmdbx.git`.
