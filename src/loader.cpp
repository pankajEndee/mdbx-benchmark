#include "loader.hpp"
#include "csv.hpp"
#include "schema.hpp"
#include "stats.hpp"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void run_bulk_load(EnvHandle& h, const LoadConfig& cfg, CsvWriter& csv,
                   CsvWriter* hist_csv) {
    for (size_t i = 0; i < DBI_COUNT; ++i) {
        const auto& spec = SCHEMA[i];
        std::cerr << "[load] dbi=" << spec.name
                  << " records=" << spec.record_count
                  << " batch=" << cfg.batch_size
                  << " append=" << (cfg.use_append ? 1 : 0) << "\n";

        Histogram hist;
        alignas(8) uint8_t key_buf[64];
        std::vector<uint8_t> val_buf(spec.val_size);

        Timer total;

        size_t written = 0;
        while (written < spec.record_count) {
            size_t batch = std::min(cfg.batch_size, spec.record_count - written);
            auto txn = h.env_for(i).start_write();
            auto cur = txn.open_cursor(h.dbis[i]);

            MDBX_put_flags_t put_flags = cfg.use_append ? MDBX_APPEND : MDBX_UPSERT;

            for (size_t b = 0; b < batch; ++b) {
                uint64_t seq = written + b;
                if (spec.flags & MDBX_INTEGERKEY)
                    make_key_int(key_buf, spec.key_size, seq);
                else
                    make_key_seq(key_buf, spec.key_size, seq);
                make_val(val_buf.data(), spec.val_size, seq);
                mdbx::slice k(key_buf, spec.key_size);
                mdbx::slice v(val_buf.data(), spec.val_size);
                MDBX_error_t rc = cur.put(k, &v, put_flags);
                if (rc != MDBX_SUCCESS) {
                    // Fall back to upsert (e.g. APPEND failed because keys are out-of-order).
                    rc = cur.put(k, &v, MDBX_UPSERT);
                    if (rc != MDBX_SUCCESS) {
                        throw std::runtime_error("cursor.put failed: " +
                            std::to_string(static_cast<int>(rc)));
                    }
                }
            }

            Timer ct;
            txn.commit();
            hist.record(ct.elapsed_ns());

            written += batch;
        }

        uint64_t elapsed_ns = total.elapsed_ns();
        double elapsed_ms = elapsed_ns / 1.0e6;
        double rps = (elapsed_ns > 0)
            ? (static_cast<double>(spec.record_count) * 1e9 /
               static_cast<double>(elapsed_ns))
            : 0.0;
        size_t db_bytes = env_file_size_bytes(h);
        double db_mb = static_cast<double>(db_bytes) / (1024.0 * 1024.0);

        csv.write_row({
            spec.name,
            std::to_string(cfg.batch_size),
            cfg.use_append ? "1" : "0",
            std::to_string(spec.record_count),
            std::to_string(elapsed_ms),
            std::to_string(rps),
            std::to_string(db_mb),
            std::to_string(hist.p50_us()),
            std::to_string(hist.p99_us()),
        });

        if (hist_csv) {
            std::string variant = std::string("load:") + spec.name
                + ":batch=" + std::to_string(cfg.batch_size)
                + ":append=" + (cfg.use_append ? "1" : "0");
            hist.write_buckets(*hist_csv, variant);
        }
    }
    print_dbi_stats(h);
}
