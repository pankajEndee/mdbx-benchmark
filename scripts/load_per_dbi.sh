#!/bin/bash
set -euo pipefail
BIN=./build/mdbx_bench
OUTDIR=./results
LAYOUT=per_dbi
mkdir -p "$OUTDIR"

DB_PATH="./bench_db_${LAYOUT}"
rm -rf "$DB_PATH"

sync
if [ -w /proc/sys/vm/drop_caches ]; then
    echo 3 > /proc/sys/vm/drop_caches
else
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
fi

sync=safe_nosync
wm=1
run_id="sync_${sync}__wm_${wm}__layout_${LAYOUT}"
echo "=== Running: $run_id ===" >&2
"$BIN" \
    --db-path "$DB_PATH" \
    --nordahead 0 \
    --dbi-map-size-gb vectors:128 \
    --out-dir "$OUTDIR" \
    --run-id  "$run_id" \
    --sync-mode "$sync" \
    --writemap  "$wm" \
    --layout    "$LAYOUT"
