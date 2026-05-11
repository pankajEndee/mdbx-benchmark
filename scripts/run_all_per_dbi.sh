#!/bin/bash
# Same matrix as run_all.sh, but with --layout per_dbi: one MDBX env per DBI.
# Each DBI gets its own subdirectory under --db-path. Output CSVs include a
# `layout` column so per_dbi rows can be compared against single-layout rows
# in the same files.
set -euo pipefail
BIN=./build/mdbx_bench
OUTDIR=./results
mkdir -p "$OUTDIR"

# Build first
cmake -B build -DCMAKE_BUILD_TYPE=Release .
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

for sync in default safe_nosync utterly_nosync; do
    for wm in 0 1; do
        run_id="sync_${sync}__wm_${wm}__layout_per_dbi"
        echo "=== Running: $run_id ===" >&2
        "$BIN" \
            --db-path "./bench_db_${run_id}" \
            --out-dir "$OUTDIR" \
            --run-id  "$run_id" \
            --sync-mode "$sync" \
            --writemap  "$wm" \
            --layout    per_dbi \
            --phase all
    done
done

echo "All per-DBI runs complete. Results in $OUTDIR/" >&2
echo "  $OUTDIR/load.csv, hot_read.csv, cold_read.csv," >&2
echo "  $OUTDIR/mixed_writer.csv, mixed_reader.csv, txn.csv, hotcold.csv" >&2
