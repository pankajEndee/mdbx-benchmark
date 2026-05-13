#!/bin/bash
# Cold-read benchmark: drops the OS page cache before every (layout, threads,
# cross_dbi, rep) tuple, so each measurement starts from disk.
#
# REQUIRES ROOT (or write access to /proc/sys/vm/drop_caches). Run via sudo.
#
# Sweeps both populated layouts (./bench_db_single and ./bench_db_per_dbi).
# Output: ./results/cold_reads.csv (appends; one row per rep).
set -euo pipefail
BIN=./build/mdbx_bench
OUTDIR=./results
mkdir -p "$OUTDIR"

THREADS="${THREADS:-1,4,8,16}"
CROSS_DBI="${CROSS_DBI:-both}"
RPT="${READS_PER_THREAD:-1000000}"
REPS="${REPS:-1}"

for LAYOUT in single per_dbi; do
    DB_PATH="./bench_db_${LAYOUT}"
    if [ ! -d "$DB_PATH" ]; then
        echo "skip: $DB_PATH does not exist (run scripts/load_${LAYOUT}.sh first)" >&2
        continue
    fi
    run_id="cold__layout_${LAYOUT}__$(date +%s)"
    echo "=== cold_reads layout=${LAYOUT} threads=${THREADS} cross_dbi=${CROSS_DBI} rpt=${RPT} reps=${REPS} ===" >&2
    "$BIN" \
        --phase cold_reads \
        --db-path "$DB_PATH" \
        --layout  "$LAYOUT" \
        --out-dir "$OUTDIR" \
        --run-id  "$run_id" \
        --threads "$THREADS" \
        --cross-dbi "$CROSS_DBI" \
        --reads-per-thread "$RPT" \
        --reps "$REPS"
done
