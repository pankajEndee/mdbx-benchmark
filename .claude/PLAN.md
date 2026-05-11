# MDBX Benchmark Suite — Execution Plan for Claude Code

## Context & Goal

Build a **self-contained C++ benchmark suite** for MDBX (libmdbx) that measures:
- Bulk load throughput
- Point read latency (hot & cold cache)
- Mixed concurrent read/write performance
- Cross-DBI atomic transaction cost
- Long-held ("cold") vs short-lived ("hot") transaction behavior

The suite must be **reproducible**, **configurable via CLI**, and **output structured CSV** for analysis.

---

## Constraints & Assumptions

- Language: **C++20** (required for `std::jthread`)
- Build system: **CMake 3.20+**
- MDBX: fetch via CMake FetchContent from `https://github.com/erthink/libmdbx`, pinned to release tag `v0.13.12` (upstream development has moved to https://gitflic.ru/project/erthink/libmdbx; the GitHub repo is a mirror but still serves tagged releases)
- Threading: `std::jthread` + `std::atomic` (no external thread library)
- No third-party benchmarking frameworks (no Google Benchmark, no Catch2)
- Target OS: **Linux** (uses `/proc/self/status` for RSS, `drop_caches` instructions printed but not automated)
- Output: **one CSV file per phase** written under `--out-dir` (e.g. `load.csv`, `hot_read.csv`, `cold_read.csv`, `mixed_writer.csv`, `mixed_reader.csv`, `txn.csv`, `hotcold.csv`). Stdout is reserved for human-readable progress logs; errors to **stderr**. Files are opened in append mode so matrix runs accumulate; rows are disambiguated by a `run_id` column.
- Single binary: `mdbx_bench`

---

## Directory Structure to Create

```
mdbx_bench/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp              # CLI parsing, phase orchestration
│   ├── schema.hpp            # DBISpec, key/value generators
│   ├── env.hpp               # MDBX env open/close, EnvConfig
│   ├── stats.hpp             # Latency histogram, MDBX stat collection
│   ├── csv.hpp               # CsvWriter: per-phase headers, common-column injection
│   ├── loader.cpp/.hpp       # Phase 1: Bulk load
│   ├── reader.cpp/.hpp       # Phase 2 & 3: Point reads (hot/cold/concurrent)
│   ├── writer.cpp/.hpp       # Phase 4: Concurrent read+write
│   ├── txn_bench.cpp/.hpp    # Phase 5: Cross-DBI txn cost
│   └── hot_cold.cpp/.hpp     # Phase 6: Hot vs cold txn behavior
└── scripts/
    └── run_all.sh            # Runs full benchmark matrix, collects CSVs
```

---

## Step 1 — CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(mdbx_bench CXX)
set(CMAKE_CXX_STANDARD 20)              # std::jthread requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")

include(FetchContent)
# Pinned to a specific release tag for reproducibility. To upgrade MDBX, bump
# the GIT_TAG below. Upstream development is on gitflic.ru; the GitHub repo is
# a mirror that still serves tagged releases.
FetchContent_Declare(
  libmdbx
  GIT_REPOSITORY https://github.com/erthink/libmdbx.git
  GIT_TAG        v0.13.12
)
set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libmdbx)

add_executable(mdbx_bench
  src/main.cpp
  src/loader.cpp
  src/reader.cpp
  src/writer.cpp
  src/txn_bench.cpp
  src/hot_cold.cpp
)
target_include_directories(mdbx_bench PRIVATE src)
target_link_libraries(mdbx_bench PRIVATE mdbx-static pthread)
```

---

## Step 2 — Schema (schema.hpp)

Define all DBIs with realistic sizes. The benchmark operates on **all DBIs simultaneously** to simulate real cross-DBI workloads.

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <random>
#include "mdbx.h++"

struct DBISpec {
    const char* name;
    size_t      key_size;     // fixed key size in bytes
    size_t      val_size;     // fixed value size in bytes
    unsigned    flags;        // MDBX_INTEGERKEY, MDBX_DUPSORT, etc.
    size_t      record_count; // target for bulk load
};

// Four DBIs with varied sizes and record counts
// Adjust record_count based on available RAM / disk
inline constexpr DBISpec SCHEMA[] = {
    {"users",    8,   128,  MDBX_INTEGERKEY,  10'000'000},
    {"sessions", 16,   64,  0,                 5'000'000},
    {"events",    8,  256,  0,                10'000'000},
    {"metadata", 32,  512,  0,                    50'000},
};
inline constexpr size_t DBI_COUNT = sizeof(SCHEMA) / sizeof(SCHEMA[0]);

// Key generator: sequential uint64 packed into key_size bytes
inline void make_key_seq(uint8_t* buf, size_t key_size, uint64_t seq) {
    memset(buf, 0, key_size);
    uint64_t be = __builtin_bswap64(seq); // big-endian for lexicographic order
    memcpy(buf, &be, std::min(sizeof(be), key_size));
}

// Key generator: random within [0, max_key)
inline void make_key_rand(uint8_t* buf, size_t key_size, uint64_t max_key, std::mt19937_64& rng) {
    uint64_t k = rng() % max_key;
    uint64_t be = __builtin_bswap64(k);
    memset(buf, 0, key_size);
    memcpy(buf, &be, std::min(sizeof(be), key_size));
}

// Zipfian key generator (precomputed CDF) for skewed hot-key workloads.
// The previous draft was O(max_key) per call, which would dominate any read
// benchmark at 10M keys and measure the generator instead of MDBX. This
// version builds the CDF once per (max_key, theta), then samples in O(log N)
// via binary search. Construction is O(N) and happens outside the timed loop;
// key generation cost must still be excluded from latency measurements (see
// "Timing discipline" below).
class ZipfGen {
    std::vector<double> cdf_;          // cumulative distribution, size = max_key
    uint64_t            max_key_ = 0;
public:
    ZipfGen() = default;
    ZipfGen(uint64_t max_key, double theta) { init(max_key, theta); }

    void init(uint64_t max_key, double theta) {
        max_key_ = max_key;
        cdf_.assign(max_key, 0.0);
        double zeta = 0.0;
        for (uint64_t i = 1; i <= max_key; ++i) {
            zeta += 1.0 / std::pow(static_cast<double>(i), theta);
            cdf_[i - 1] = zeta;
        }
        for (auto& c : cdf_) c /= zeta;   // normalize
    }

    uint64_t sample(std::mt19937_64& rng) const {
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<uint64_t>(std::distance(cdf_.begin(), it));
    }
};

// Value filler: deterministic based on key so reads can be verified
inline void make_val(uint8_t* buf, size_t val_size, uint64_t key_seq) {
    memset(buf, (uint8_t)(key_seq & 0xFF), val_size);
    memcpy(buf, &key_seq, std::min(sizeof(key_seq), val_size));
}
```

---

## Step 3 — Environment Config (env.hpp)

```cpp
#pragma once
#include "mdbx.h++"
#include <string>

struct EnvConfig {
    std::string  path         = "./bench_db";
    size_t       map_size     = size_t(64) * 1024 * 1024 * 1024; // 64 GB
    unsigned     max_readers  = 128;
    unsigned     max_dbi      = 16;
    // Flags to vary between benchmark runs
    bool         writemap     = true;   // MDBX_WRITEMAP
    bool         nordahead    = true;   // MDBX_NORDAHEAD
    bool         liforeclaim  = true;   // MDBX_LIFORECLAIM
    // Sync mode: "default", "safe_nosync", "utterly_nosync"
    std::string  sync_mode    = "safe_nosync";
};

// Opens env, sets map size, opens all DBIs in SCHEMA[]
// Returns: env handle, array of DBI handles (size DBI_COUNT)
struct EnvHandle {
    mdbx::env_managed          env;
    mdbx::map_handle           dbis[DBI_COUNT];
};

EnvHandle open_env(const EnvConfig& cfg);
void      close_env(EnvHandle& h);
void      print_env_info(EnvHandle& h);  // prints MDBX_envinfo fields to stderr
void      print_dbi_stats(EnvHandle& h); // prints MDBX_stat for each DBI to stderr
```

**Implementation notes for env.hpp:**
- Use `mdbx::env::operate_parameters` to set flags
- Open each DBI from `SCHEMA[]` in a write txn during init
- `print_env_info` should dump: map_size, last_pgno, recent_txnid, num_readers, geo fields
- `print_dbi_stats` should dump per-DBI: ms_branch_pages, ms_leaf_pages, ms_overflow_pages, ms_entries, ms_depth

---

## Step 4 — Latency Histogram (stats.hpp)

```cpp
#pragma once
#include <cstdint>
#include <array>
#include <chrono>
#include <string>

// HDR-style power-of-2 bucket histogram, nanosecond resolution
// Buckets: [0,1us), [1us,10us), [10us,100us), [100us,1ms), [1ms,10ms), [10ms,+)
struct Histogram {
    std::array<uint64_t, 64> buckets{};
    uint64_t total_count = 0;
    uint64_t total_ns    = 0;
    uint64_t min_ns      = UINT64_MAX;
    uint64_t max_ns      = 0;

    void record(uint64_t ns);
    uint64_t percentile(double p) const; // p in [0,100]
    double   mean_us()            const;
    // Emit summary stats as a row through CsvWriter (see Step 4b). Per-phase
    // headers are defined centrally in csv.hpp so call sites stay declarative.
    void     write_summary(class CsvWriter& w, const std::string& variant) const;
    // Optional full-resolution dump: one row per non-empty bucket. Gated by
    // --dump-histograms on the CLI. Written to "<phase>_hist.csv".
    void     write_buckets(class CsvWriter& w, const std::string& variant) const;
};

// RAII timer
struct Timer {
    using Clock = std::chrono::steady_clock;
    Clock::time_point start = Clock::now();
    uint64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count();
    }
};

// Process RSS in KB (reads /proc/self/status)
size_t get_rss_kb();
```

---

## Step 4b — CSV Writer (csv.hpp)

Centralized CSV output. Every phase writes to its **own file** under `--out-dir`, and every row carries the same set of common columns describing the run.

```cpp
#pragma once
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <filesystem>

// Common columns prepended to every row, regardless of phase.
struct RunMeta {
    std::string run_id;        // from --run-id (e.g. "sync_safe_nosync__wm_1")
    std::string timestamp_iso; // ISO-8601 UTC, set at process start
    std::string sync_mode;
    int         writemap    = 0;
    int         nordahead   = 0;
    int         liforeclaim = 0;
};

// Stable per-phase header lists. The phase-specific columns follow the
// common columns above. Keep these in sync with the documented columns in
// Steps 5–9 below.
inline const std::unordered_map<std::string, std::vector<std::string>> PHASE_COLUMNS = {
    {"load",         {"dbi", "batch_size", "use_append", "total_records",
                      "elapsed_ms", "records_per_sec", "db_size_mb",
                      "commit_p50_us", "commit_p99_us"}},
    {"hot_read",     {"pattern", "threads", "cross_dbi", "total_ops",
                      "elapsed_ms", "ops_per_sec",
                      "p50_us", "p95_us", "p99_us"}},
    {"cold_read",    {"pattern", "threads", "cross_dbi", "total_ops",
                      "elapsed_ms", "ops_per_sec",
                      "p50_us", "p95_us", "p99_us"}},
    {"mixed_writer", {"writer_batch", "reader_threads", "write_txns",
                      "commits_per_sec", "p50_commit_us", "p99_commit_us"}},
    {"mixed_reader", {"reader_threads", "writer_batch", "total_read_ops",
                      "read_ops_per_sec", "p50_read_us", "p99_read_us"}},
    {"txn",          {"atomic", "dbi_count", "records_per_dbi", "txn_count",
                      "commits_per_sec", "p50_us", "p99_us"}},
    {"hotcold",      {"sample_idx", "writer_txnid", "oldest_reader_txnid",
                      "lag", "db_size_mb", "writer_p99_us"}},
};

// One CsvWriter per phase. Opens the file in append mode; writes the header
// only if the file is new/empty. Each write_row() call prepends RunMeta.
class CsvWriter {
public:
    CsvWriter(const std::filesystem::path& out_dir,
              std::string phase,
              const RunMeta& meta);

    // values.size() must equal PHASE_COLUMNS.at(phase).size()
    void write_row(const std::vector<std::string>& values);

private:
    std::ofstream out_;
    std::string   phase_;
    RunMeta       meta_;
    void write_header_if_new(const std::filesystem::path& path);
};
```

**Implementation notes:**
- Append mode + header-only-if-new lets `scripts/run_all.sh` invoke the binary repeatedly with different `--run-id` values; the resulting `results/<phase>.csv` files accumulate every matrix run.
- Quote values containing commas/quotes per RFC 4180; numeric columns never need quoting.
- `timestamp_iso` is captured once at process start (not per row) so all rows from one invocation share a timestamp.
- For `--dump-histograms`, a parallel `CsvWriter` is constructed against `<phase>_hist.csv` with a fixed schema: `bucket_lo_ns, bucket_hi_ns, count`.

---

## Step 5 — Phase 1: Bulk Loader (loader.hpp / loader.cpp)

### Purpose
Load all DBIs to their target `record_count`. Measure throughput and file size growth.

### Interface
```cpp
struct LoadConfig {
    size_t batch_size   = 10'000;   // records per commit
    bool   use_append   = true;     // MDBX_APPEND (requires sequential keys)
    bool   use_cursor   = true;     // cursor_put vs put
};

void run_bulk_load(EnvHandle& h, const LoadConfig& cfg);
```

### Behavior
1. For each DBI in `SCHEMA[]`:
   - Open a write txn
   - Open a cursor
   - Insert records `[0, record_count)` with sequential keys (`make_key_seq`)
   - Commit every `batch_size` records
   - Time each commit; record in histogram
2. After all DBIs loaded, call `print_dbi_stats`
3. Append one row per DBI to `<out_dir>/load.csv` via `CsvWriter`. Common columns (`run_id, timestamp, sync_mode, writemap, nordahead, liforeclaim`) are prepended automatically; phase-specific columns are:
   ```
   dbi, batch_size, use_append, total_records, elapsed_ms, records_per_sec, db_size_mb, commit_p50_us, commit_p99_us
   ```

### Variants to run (main.cpp will iterate):
| batch_size | use_append |
|---|---|
| 1 | false |
| 1000 | false |
| 10000 | true |
| 100000 | true |

---

## Step 6 — Phase 2 & 3: Point Reads (reader.hpp / reader.cpp)

### Purpose
Measure single-key lookup latency under hot (warm page cache) and cold (after cache drop) conditions.

### Interface
```cpp
enum class KeyPattern { Sequential, Random, Zipfian };

struct ReadConfig {
    size_t      ops_per_thread = 1'000'000;
    KeyPattern  pattern        = KeyPattern::Random;
    unsigned    thread_count   = 1;
    bool        cross_dbi      = false; // read from all DBIs in one txn
};

void run_point_reads(EnvHandle& h, const ReadConfig& cfg, const std::string& phase_name);
```

### Behavior
1. Spawn `thread_count` reader threads
2. Each thread:
   - Starts a read txn (`mdbx::txn_managed` in read mode)
   - Does `ops_per_thread / thread_count` lookups
   - For `cross_dbi=true`: each op reads one key from **each** DBI in one txn
   - Records latency per op in thread-local histogram
   - Commits (aborts) read txn; renews immediately
3. Join threads, merge histograms
4. Append one row per variant to `<out_dir>/hot_read.csv` or `<out_dir>/cold_read.csv` (selected by the `phase_name` argument) via `CsvWriter`. Phase-specific columns (after the common columns):
   ```
   pattern, threads, cross_dbi, total_ops, elapsed_ms, ops_per_sec, p50_us, p95_us, p99_us
   ```

### Phases:
- **Phase 2 (Hot)**: Run immediately after bulk load (page cache warm). `phase_name = "hot_read"`.
- **Phase 3 (Cold)**: `main.cpp` prints the instruction to run `echo 3 > /proc/sys/vm/drop_caches` as root, then calls `close_env()` and `open_env()` to reopen with a fresh MDBX state before re-running the same variants. `phase_name = "cold_read"`.

### Variants:
| pattern | threads | cross_dbi |
|---|---|---|
| Random | 1 | false |
| Random | 4 | false |
| Random | 16 | false |
| Zipfian | 8 | false |
| Random | 8 | true |

---

## Step 7 — Phase 4: Concurrent Read + Write (writer.hpp / writer.cpp)

### Purpose
Measure writer throughput and reader latency degradation under concurrent write load. This is MDBX's key bottleneck (single writer).

### Interface
```cpp
struct MixedConfig {
    unsigned reader_threads  = 8;
    unsigned writer_batch    = 1'000;   // records per write txn
    size_t   write_txns      = 10'000;  // total write txns to run
    bool     cross_dbi_write = true;    // each write txn touches all DBIs
    KeyPattern read_pattern  = KeyPattern::Random;
};

void run_mixed(EnvHandle& h, const MixedConfig& cfg);
```

### Behavior
1. Start `reader_threads` reader threads (each loops: open read txn → random gets → renew)
2. Start **1 writer thread**:
   - Each write txn: for each DBI, put `writer_batch / DBI_COUNT` records with random keys
   - Commit; record commit latency
   - Repeat `write_txns` times
3. Readers track ops/sec and p99 latency independently
4. After writer finishes, signal readers to stop
5. Emit one row to **each** of two files, joined by `run_id` for analysis:
   - `<out_dir>/mixed_writer.csv` — phase-specific columns:
     ```
     writer_batch, reader_threads, write_txns, commits_per_sec, p50_commit_us, p99_commit_us
     ```
   - `<out_dir>/mixed_reader.csv` — phase-specific columns (aggregated across reader threads):
     ```
     reader_threads, writer_batch, total_read_ops, read_ops_per_sec, p50_read_us, p99_read_us
     ```

### Variants:
| reader_threads | writer_batch | cross_dbi_write |
|---|---|---|
| 0 | 1000 | true |
| 4 | 1000 | true |
| 8 | 1000 | true |
| 16 | 1000 | true |
| 8 | 100 | true |
| 8 | 10000 | true |

---

## Step 8 — Phase 5: Cross-DBI Transaction Cost (txn_bench.hpp / txn_bench.cpp)

### Purpose
Isolate the cost of touching N DBIs in one atomic write transaction vs N separate transactions.

### Interface
```cpp
struct TxnBenchConfig {
    size_t  txn_count    = 100'000;
    size_t  records_per_dbi_per_txn = 10;
    bool    atomic       = true; // true = all DBIs in one txn, false = one txn per DBI
};

void run_txn_bench(EnvHandle& h, const TxnBenchConfig& cfg);
```

### Behavior
1. For `atomic=true`: single write txn touches all DBIs
2. For `atomic=false`: separate sequential write txns per DBI
3. Measure: txn commit latency, total elapsed, effective records/sec
4. Append one row per variant to `<out_dir>/txn.csv`. Phase-specific columns:
   ```
   atomic, dbi_count, records_per_dbi, txn_count, commits_per_sec, p50_us, p99_us
   ```

---

## Step 9 — Phase 6: Hot vs Cold Transactions (hot_cold.hpp / hot_cold.cpp)

### Purpose
Simulate a long-held reader txn blocking page reclaim while a writer runs. Measure: writer slowdown, DB file growth (page leakage), and time to reclaim after reader releases.

### Interface
```cpp
struct HotColdConfig {
    uint64_t writer_txns       = 50'000;
    size_t   writer_batch      = 500;
    unsigned stale_reader_hold_ms = 5000; // how long reader holds txn open
};

void run_hot_cold(EnvHandle& h, const HotColdConfig& cfg);
```

### Behavior
1. Start writer thread: continuously commits write txns
2. Start "stale reader" thread:
   - Opens a read txn
   - Sleeps `stale_reader_hold_ms`
   - Releases txn
   - Repeats
3. Every 1000 writer commits, sample:
   - `mdbx_env_info`: `mi_latter_reader_txnid`, `mi_recent_txnid` (lag = difference)
   - DB file size on disk
   - Writer commit p99 latency in that window
4. After stale reader releases, measure how quickly lag closes
5. Append one row per sample to `<out_dir>/hotcold.csv` (time-series). Phase-specific columns:
   ```
   sample_idx, writer_txnid, oldest_reader_txnid, lag, db_size_mb, writer_p99_us
   ```

---

## Step 10 — Main Orchestrator (main.cpp)

### CLI Interface
```
Usage: mdbx_bench [options]

Options:
  --db-path <path>          Default: ./bench_db
  --out-dir <path>          Directory for per-phase CSV files. Default: ./results
  --run-id <string>         Tag written into the run_id column of every row.
                            Default: auto-generated from sync-mode + writemap + timestamp.
  --map-size-gb <n>         Default: 64
  --writemap <0|1>          Default: 1
  --nordahead <0|1>         Default: 1
  --liforeclaim <0|1>       Default: 1
  --sync-mode <s>           default|safe_nosync|utterly_nosync  Default: safe_nosync
  --phase <name>            load|hot_read|cold_read|mixed|txn|hotcold|all  Default: all
  --threads <n>             Reader thread count override
  --batch-size <n>          Writer batch size override
  --ops <n>                 Read ops per thread override
  --no-load                 Skip bulk load (use existing DB)
  --dump-histograms         Also emit <phase>_hist.csv with one row per histogram bucket
  --help
```

### Execution Order (for `--phase all`)
1. Open env with config
2. Phase 1: Bulk load (all batch variants)
3. Phase 2: Hot reads (all variants)
4. Print instructions for cache drop, wait for Enter
5. Phase 3: Cold reads (same variants as hot)
6. Phase 4: Mixed read/write (all variants)
7. Phase 5: Cross-DBI txn cost
8. Phase 6: Hot vs cold txn

### Output format
Each phase appends to its own CSV file under `--out-dir`. Files share a stable, phase-specific schema (see Step 4b) and start with the common run-identifying columns: `run_id, timestamp, sync_mode, writemap, nordahead, liforeclaim`. Stdout is reserved for human-readable progress logs; redirecting stdout to a `.csv` is no longer how results are captured.

```bash
./mdbx_bench \
    --sync-mode safe_nosync --writemap 1 \
    --out-dir ./results \
    --run-id safe_nosync_wm1
# → ./results/{load,hot_read,cold_read,mixed_writer,mixed_reader,txn,hotcold}.csv
```

Re-running with a different `--run-id` appends new rows to the same files; analysis tools group by `run_id`.

---

## Step 11 — Benchmark Matrix Shell Script (scripts/run_all.sh)

```bash
#!/bin/bash
set -euo pipefail
BIN=./build/mdbx_bench
OUTDIR=./results
mkdir -p "$OUTDIR"

# Build first
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j"$(nproc)"

# Matrix: vary sync mode and writemap. Each invocation appends rows to the
# shared per-phase files in $OUTDIR; the run_id column disambiguates them.
for sync in default safe_nosync utterly_nosync; do
    for wm in 0 1; do
        run_id="sync_${sync}__wm_${wm}"
        echo "=== Running: $run_id ===" >&2
        "$BIN" \
            --db-path "./bench_db_${run_id}" \
            --out-dir "$OUTDIR" \
            --run-id  "$run_id" \
            --sync-mode "$sync" \
            --writemap  "$wm" \
            --phase all
    done
done

echo "All runs complete. Results in $OUTDIR/" >&2
echo "  $OUTDIR/load.csv, hot_read.csv, cold_read.csv," >&2
echo "  $OUTDIR/mixed_writer.csv, mixed_reader.csv, txn.csv, hotcold.csv" >&2
```

---

## Step 12 — README.md

Include:
1. Build instructions (cmake + make)
2. How to run single phase vs full matrix
3. How to drop page cache between hot/cold phases
4. Description of each CSV output column
5. What each phase measures and why
6. Table of env flags and their tradeoffs

---

## Implementation Notes for Claude Code

### MDBX C++ API style to use
Use the `mdbx.h++` C++ wrapper throughout (not raw C API):
```cpp
#include "mdbx.h++"
// env: mdbx::env_managed
// txn: mdbx::txn_managed
// cursor: mdbx::cursor_managed
// map: mdbx::map_handle
// slice: mdbx::slice (wraps key/val buffers)
```

### Thread safety rules
- One `mdbx::txn_managed` per thread — never share txns across threads
- Readers: each thread opens its own read txn, renews with `txn.renew()`
- Writer: single writer thread only; all write txns serialized
- `EnvHandle` struct is shared read-only across threads after init

### Error handling
- Wrap all MDBX calls in try/catch for `mdbx::exception`
- On `MDBX_READERS_FULL`: print to stderr and abort that thread's op, continue
- On `MDBX_MAP_FULL`: abort benchmark, print resize instruction

### Key alignment
- Allocate key/value buffers with `alignas(8)` for MDBX_INTEGERKEY DBIs
- Reuse buffers per thread to avoid alloc overhead in hot loops

### Timing discipline
- Time **only** the MDBX operation, not key generation
- For batch commits: time the `txn.commit()` call only, not the puts
- Use `CLOCK_MONOTONIC_RAW` via `clock_gettime` or `std::chrono::steady_clock`

### What NOT to do
- Do not use `MDBX_UTTERLY_NOSYNC` for correctness testing — only throughput ceiling
- Do not share cursors across transactions
- Do not hold write txns open across thread boundaries
- Do not measure latency using `gettimeofday` (not monotonic)

---

## Expected Output Files

After `run_all.sh`:
```
results/
  load.csv          # Phase 1
  hot_read.csv      # Phase 2
  cold_read.csv     # Phase 3
  mixed_writer.csv  # Phase 4 (writer-side metrics)
  mixed_reader.csv  # Phase 4 (reader-side metrics)
  txn.csv           # Phase 5
  hotcold.csv       # Phase 6 (time-series, one row per sample)
```

Each file has a stable phase-specific schema. Rows from all six matrix runs (3 sync modes × 2 writemap settings) accumulate in the same files; group/filter by the `run_id` column for analysis. If `--dump-histograms` is passed, a matching `<phase>_hist.csv` is produced alongside each summary file.

---

## Acceptance Criteria

The implementation is complete when:
- [ ] `cmake --build build` succeeds with `-O3 -march=native`
- [ ] `./mdbx_bench --phase load` loads 10M+ records into `users` DBI without error
- [ ] `./mdbx_bench --phase hot_read --threads 16` runs 16 concurrent readers without deadlock
- [ ] `./mdbx_bench --phase mixed --threads 8` runs writer + 8 readers concurrently
- [ ] Each phase writes a CSV file under `--out-dir` with the documented column header on first creation, and appends rows on subsequent runs without rewriting the header
- [ ] Every row in every phase file carries the common columns (`run_id, timestamp, sync_mode, writemap, nordahead, liforeclaim`), and matrix runs with distinct `--run-id` values coexist in the same files
- [ ] `run_all.sh` completes the full matrix without intervention (except cache drop prompt)
- [ ] No MDBX_MAP_FULL errors (map size pre-set large enough)
- [ ] Cold-read p99 is meaningfully higher than hot-read p99 for the same variant (validates that the cache-drop + env-reopen methodology actually defeats the page cache; a small or zero gap means the cold phase isn't really cold)