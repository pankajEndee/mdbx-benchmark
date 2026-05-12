#!/bin/bash
# Same matrix as run_all.sh, but with --layout per_dbi: one MDBX env per DBI.
# Each DBI gets its own subdirectory under --db-path. Output CSVs include a
# `layout` column so per_dbi rows can be compared against single-layout rows
# in the same files.
set -euo pipefail
BIN=./build/mdbx_bench
OUTDIR=./results
PHASE=load
mkdir -p "$OUTDIR"

# Build first
# cmake -B build -DCMAKE_BUILD_TYPE=Release .
# cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

sync=safe_nosync
wm=1
run_id="sync_${sync}__wm_${wm}__layout_per_dbi"
echo "=== Running: $run_id ===" >&2
"$BIN" \
    --db-path "./bench_db" \
    --dbi-map-size-gb vectors:256 \
    --map-size-gb 64 \
    --out-dir "$OUTDIR" \
    --run-id  "$run_id" \
    --sync-mode "$sync" \
    --writemap  "$wm" \
    --layout    per_dbi \
    --phase "$PHASE"

echo "All per-DBI runs complete. Results in $OUTDIR/" >&2
echo "  $OUTDIR/load.csv, hot_read.csv, cold_read.csv," >&2
echo "  $OUTDIR/mixed_writer.csv, mixed_reader.csv, txn.csv, hotcold.csv" >&2
