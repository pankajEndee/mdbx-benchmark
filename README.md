# MDBX Benchmark Suite

A self-contained C++20 benchmark suite for [libmdbx](https://github.com/erthink/libmdbx) that measures bulk-load throughput, point-read latency (hot & cold cache), mixed read/write performance, cross-DBI transaction cost, and the impact of long-held reader transactions on a writer.

The suite supports **two storage layouts**:
- `single` (default): one MDBX env with multiple DBIs inside it.
- `per_dbi`: one MDBX env *per DBI* (separate directory, file, and lock per DBI).

Both layouts run the same phases against the same schema, so the CSVs from both can be compared directly. The suite is reproducible (MDBX is pinned via CMake `FetchContent`), configurable via CLI flags, and emits one structured CSV per phase so a matrix of runs can be analyzed together.

## Contents

- [Requirements](#requirements)
- [Building](#building)
- [Quick start](#quick-start)
- [Full benchmark matrix](#full-benchmark-matrix)
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
| Compiler | C++20 (GCC 11+, Clang 14+) | `std::jthread` and `<stop_token>` are required |
| CMake | 3.20+ | `FetchContent_MakeAvailable` |
| Git | any | needed by `FetchContent` to clone libmdbx |
| OS | Linux preferred | `/proc/self/status` is used for RSS, and `drop_caches` exists only on Linux. The code compiles on macOS but the cold-read methodology cannot defeat the page cache there |
| Disk | ~20 GB free | The default schema loads ~25 M records totalling several GB; the map size is reserved at 64 GB |
| RAM | 8 GB minimum, 16+ recommended | Hot-read measurements assume the working set fits in RAM |

MDBX itself has no external runtime dependencies. The build links it statically.

## Building

```bash
# from the repo root
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j"$(nproc)"
# binary: ./build/mdbx_bench
```

The first configure step clones libmdbx at the pinned tag (`v0.13.12`) into `build/_deps/libmdbx-src`. Subsequent configures reuse the cache.

To upgrade MDBX, edit `GIT_TAG` in [CMakeLists.txt](CMakeLists.txt) and delete `build/_deps/libmdbx-*`.

## Quick start

```bash
# 1. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release . && cmake --build build -j

# 2. Run a single phase against a fresh DB (single-env layout)
./build/mdbx_bench \
    --db-path ./bench_db \
    --out-dir ./results \
    --run-id  quickstart \
    --phase   load

# 2b. Same load, but with one MDBX env per DBI
./build/mdbx_bench \
    --db-path ./bench_db_per_dbi \
    --out-dir ./results \
    --run-id  quickstart_per_dbi \
    --layout  per_dbi \
    --phase   load

# 3. Inspect output
ls results/
#   load.csv
column -ts, < results/load.csv | less -S
```

Run `./build/mdbx_bench --help` for the full flag list.

### Run an individual phase

Each phase can be invoked on its own. After the initial `load` phase has populated `--db-path`, subsequent phases can be run with `--no-load` (or simply by passing `--phase <name>`, which never re-loads).

```bash
# Load once (slow), then iterate on read variants
./build/mdbx_bench --db-path ./bench_db --out-dir ./results --run-id wm1 --phase load
./build/mdbx_bench --db-path ./bench_db --out-dir ./results --run-id wm1 --phase hot_read
./build/mdbx_bench --db-path ./bench_db --out-dir ./results --run-id wm1 --phase mixed
./build/mdbx_bench --db-path ./bench_db --out-dir ./results --run-id wm1 --phase txn
./build/mdbx_bench --db-path ./bench_db --out-dir ./results --run-id wm1 --phase hotcold
```

### Run the full sequence (`--phase all`)

`--phase all` runs load → hot_read → cold_read → mixed → txn → hotcold in order. Before cold reads it prompts you to drop the page cache (root required) and presses on after you hit Enter:

```bash
sudo sysctl vm.drop_caches=3   # in another terminal, when prompted
# or: sudo sh -c 'sync && echo 3 > /proc/sys/vm/drop_caches'
```

On macOS, `purge` is the closest equivalent but does not give the same guarantee.

## Full benchmark matrix

Two scripts are provided. Each sweeps `sync_mode ∈ {default, safe_nosync, utterly_nosync}` × `writemap ∈ {0, 1}` = 6 runs. Each run uses its own `--db-path` (so prior loads don't bleed across configurations) but appends to the same per-phase CSV files in `./results/`. Rows are disambiguated by the `run_id` and `layout` columns.

```bash
./scripts/run_all.sh           # 6 runs with --layout single  (one env, many DBIs)
./scripts/run_all_per_dbi.sh   # 6 runs with --layout per_dbi (one env per DBI)
# → ./results/{load,hot_read,cold_read,mixed_writer,mixed_reader,txn,hotcold}.csv
```

Running both scripts back-to-back gives you a 12-row comparison per phase variant, with the `layout` column distinguishing the two configurations.

Expect each script to take **several hours** end-to-end, plus six manual cache-drop prompts. To run unattended on a Linux box, pre-authorize `sudo sysctl` and replace the `getline` prompt in `main.cpp` (or comment it out).

## Storage layouts

The `--layout` flag selects how DBIs are arranged on disk:

| Layout | Disk shape | Atomicity | Concurrency | Use case |
|---|---|---|---|---|
| `single` (default) | `<db-path>/mdbx.dat` + `mdbx.lck` with N DBIs inside one env | A single write txn can touch all DBIs atomically | One writer for the whole env; readers serialize on env's reader slots | Most production setups |
| `per_dbi` | `<db-path>/<dbi_name>/mdbx.dat` + `mdbx.lck` per DBI; N independent envs | No cross-DBI atomicity — each env commits independently | Each env has its own writer slot, so DBIs can be written in parallel | Workloads where DBIs are functionally independent and you want to remove the global write lock |

In `per_dbi` mode the `--map-size-gb` budget is split evenly across the N envs (each env gets `map_size / DBI_COUNT`). On-disk size grows dynamically, so this is just an upper-bound reservation.

### Per-DBI mode caveats

- **Cross-DBI atomic txn is emulated.** The `txn` phase with `atomic=1` opens N write txns (one per env), writes into each, then commits them sequentially. The commit times are summed and recorded under the same column as single-layout atomic commits, but they are **not** atomic across envs — a crash between commits can leave the set inconsistent. The log line `[txn] note: per-DBI atomic is emulated ...` flags this.
- **Cross-DBI reads use N read txns.** The hot/cold read phases and the mixed reader allocate one read txn per env when `cross_dbi=1`, so the snapshot is not globally consistent across DBIs.
- **The mixed writer commits N times per logical "write txn"** when `cross_dbi_write=1`, since each env has its own writer.
- **The `hotcold` stale reader** holds a read txn open in **every** env, so page reclaim is blocked across the full multi-env set (matching the single-env behavior where one read txn pins all DBIs).
- **Aggregated lag**: `hotcold` reports `writer_txnid` = max over all envs and `oldest_reader_txnid` = min over all envs.

## CLI reference

```
Usage: mdbx_bench [options]

Options:
  --db-path <path>          Default: ./bench_db
  --out-dir <path>          Directory for per-phase CSV files. Default: ./results
  --run-id <string>         Tag written into the run_id column of every row.
                            Default: auto-generated from sync-mode + writemap + timestamp.
  --map-size-gb <n>         Default: 64
  --writemap <0|1>          Default: 1   (MDBX_WRITEMAP)
  --nordahead <0|1>         Default: 1   (MDBX_NORDAHEAD)
  --liforeclaim <0|1>       Default: 1   (MDBX_LIFORECLAIM)
  --sync-mode <s>           default | safe_nosync | utterly_nosync   Default: safe_nosync
  --layout <s>              single | per_dbi   Default: single
                            per_dbi opens one MDBX env per DBI; map-size is split evenly.
  --phase <name>            load | hot_read | cold_read | mixed | txn | hotcold | all
                            Default: all
  --threads <n>             Reader thread count override (read & mixed phases)
  --batch-size <n>          Writer batch size override (load & mixed phases)
  --ops <n>                 Read ops per thread override
  --no-load                 Skip bulk load (use existing DB)
  --dump-histograms         Also emit <phase>_hist.csv with one row per non-empty bucket
  --help
```

If you re-run with the same `--out-dir` but a different `--run-id`, rows are **appended** to the existing CSV files; the header is written only when the file is first created. This is what lets `run_all.sh` aggregate six matrix runs into a single set of files.

## Phases

| Phase | What it measures | Why it matters |
|---|---|---|
| `load` | Bulk-insert throughput and commit p50/p99 across 4 DBIs (~25 M total records) at various batch sizes, with and without `MDBX_APPEND` | Reveals the cost of small-batch commits vs amortized large-batch loads, and the throughput win of in-order appends |
| `hot_read` | Single-key lookup latency with a warm page cache (run immediately after load) at 1/4/16 reader threads, random vs Zipfian, single-DBI vs cross-DBI | This is the latency you actually see in production once the working set is resident |
| `cold_read` | Same variants as `hot_read`, but after dropping the OS page cache and reopening the env | Isolates disk-read cost. The hot vs cold gap is your effective cache benefit |
| `mixed` | Single-writer commit rate **and** concurrent reader p50/p99 under varying reader counts and write batch sizes | MDBX serializes writers, so the question is "how much does a writer hurt readers" — this answers it |
| `txn` | Cost of touching N DBIs in **one** atomic write transaction vs **N** sequential write transactions | Tells you the price of multi-DBI atomicity; informs whether to coalesce updates |
| `hotcold` | Time-series of (writer txnid, oldest-reader txnid, lag, DB size, writer p99) while a stale long-held reader transaction blocks page reclaim | Demonstrates how a single misbehaving reader causes uncontrolled DB-file growth and writer slowdown, and how fast lag closes once the reader releases |

The default schema ([src/schema.hpp](src/schema.hpp)) is seven DBIs sized to roughly resemble a real workload:

| DBI | key size | val size | records | flags |
|---|---|---|---|---|
| `vectors` | 4 B | 1540 B | 10 000 | `MDBX_INTEGERKEY` |
| `meta` | 4 B | 75 B | 10 000 | — |
| `id` | 7 B | 4 B | 10 000 | — |
| `filter_schema` | 17 B | 48 B | 5 | — |
| `filter_numeric_inverted` | 7 B | 10 000 B | 14 | — |
| `filter_numeric_forward` | 10 B | 4 B | 10 000 | — |
| `filter_category` | 16 B | 2 B | 6 | — |

Edit `SCHEMA[]` to change sizes. Cross-DBI phases operate on **all** DBIs in one transaction (single layout) or one txn per env (per-DBI layout).

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

### `hot_read.csv` / `cold_read.csv` — one row per (pattern, threads, cross_dbi) variant
```
pattern, threads, cross_dbi, total_ops,
elapsed_ms, ops_per_sec, p50_us, p95_us, p99_us
```
- `total_ops = ops_per_thread × threads`.
- Latency is per-op for single-DBI reads; for `cross_dbi=1` it is per-op-across-all-DBIs.

### `mixed_writer.csv` — one row per writer variant
```
writer_batch, reader_threads, write_txns,
commits_per_sec, p50_commit_us, p99_commit_us
```

### `mixed_reader.csv` — one row per writer variant, aggregated across all reader threads
```
reader_threads, writer_batch, total_read_ops,
read_ops_per_sec, p50_read_us, p99_read_us
```
The two `mixed_*` files share a `run_id` per variant, so they can be joined for analysis.

### `txn.csv` — one row per (atomic ∈ {true, false}) configuration
```
atomic, dbi_count, records_per_dbi, txn_count,
commits_per_sec, p50_us, p99_us
```

### `hotcold.csv` — time-series, one row per 1000-commit window plus a short cool-down
```
sample_idx, writer_txnid, oldest_reader_txnid,
lag, db_size_mb, writer_p99_us
```
- `lag = writer_txnid − oldest_reader_txnid`. While the stale reader is held it should grow steadily; after it releases, `lag` collapses and `db_size_mb` should stop growing (or shrink on next compaction).

### `<phase>_hist.csv` (when `--dump-histograms` is passed)
Raw histogram dump alongside the summary file:
```
variant, bucket_lo_ns, bucket_hi_ns, count
```
Buckets are power-of-two in nanoseconds; only non-empty buckets are emitted.

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

The matrix script sweeps `sync_mode` × `writemap` so you can quantify each flag's contribution.

## Methodology notes

- **Timer**: `std::chrono::steady_clock` (monotonic). Per-op timing measures only the MDBX call; key generation is excluded from timed regions.
- **Histograms**: Power-of-2 nanosecond buckets, 64 buckets total. Percentiles return the upper edge of the containing bucket, which is conservative.
- **Zipfian generator**: CDF is precomputed once per run (O(N) construction), then sampled in O(log N). This is built **outside** the timed loop so it doesn't pollute read latency.
- **Cold reads**: After hot reads, `main.cpp` calls `close_env()` and re-`open_env()` so MDBX's internal caches are fresh. You must additionally drop the OS page cache for the methodology to be honest — otherwise the "cold" phase isn't cold. The suite verifies this implicitly: cold p99 should be meaningfully higher than hot p99.
- **Mixed phase**: Exactly one writer thread (MDBX is single-writer). Reader threads run until the writer finishes its configured number of transactions, then are stopped via `std::stop_token`.
- **Threading**: One `mdbx::txn_managed` per thread; never shared. Each reader opens its own read txn and renews it periodically (every 64 K ops) to release page-snapshot pins.
- **`ops_per_sec` in `mixed_reader.csv`** is computed against the writer's wall clock, since readers run for the writer's lifetime.

## Troubleshooting

### `MDBX_MAP_FULL`
Increase `--map-size-gb`. The default is 64 GB of address-space reservation (not disk); on a 32-bit system or with `ulimit -v` set this can fail.

### `MDBX_READERS_FULL`
Increase `max_readers` in [src/env.hpp](src/env.hpp) (default 128). Each `std::jthread` reader owns one slot.

### `MDBX_APPEND` errors during load
The loader catches `mdbx::exception` from `cur.put(..., MDBX_APPEND)` and falls back to `MDBX_UPSERT`. If you see many fallbacks, your key generator is producing out-of-order keys.

### Hot reads and cold reads have similar p99
Either your working set fits entirely in L3/RAM (rare with 25 M records) or the cache drop didn't take. Verify with `free -h` before and after `echo 3 > /proc/sys/vm/drop_caches`.

### Build can't fetch libmdbx
The repo URL pinned in `CMakeLists.txt` is the GitHub mirror. Upstream development has moved to gitflic.ru; the GitHub mirror still serves tagged releases. If GitHub is unreachable, point `GIT_REPOSITORY` at `https://gitflic.ru/project/erthink/libmdbx.git`.

### `std::jthread` not found
Your compiler's standard library predates C++20 jthread support (libc++ on older Apple Clang is a common culprit). Use GCC 11+ or upgrade Xcode/libc++.
