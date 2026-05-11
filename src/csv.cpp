#include "csv.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

const std::unordered_map<std::string, std::vector<std::string>> PHASE_COLUMNS = {
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

static const std::vector<std::string> kCommonColumns = {
    "run_id", "timestamp", "sync_mode", "writemap", "nordahead", "liforeclaim", "layout"
};

std::string csv_escape(std::string_view s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') { needs_quote = true; break; }
    }
    if (!needs_quote) return std::string(s);
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string iso8601_utc_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream os;
    os << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

CsvWriter::CsvWriter(const std::filesystem::path& out_dir,
                     std::string phase,
                     const RunMeta& meta)
    : phase_(std::move(phase)), meta_(meta) {
    std::filesystem::create_directories(out_dir);
    // Distinguish phase output vs raw (histogram) output by suffix.
    bool is_hist = phase_.size() > 5 &&
                   phase_.compare(phase_.size() - 5, 5, "_hist") == 0;
    common_prefix_ = !is_hist;
    auto path = out_dir / (phase_ + ".csv");
    bool exists = std::filesystem::exists(path) &&
                  std::filesystem::file_size(path) > 0;
    out_.open(path, std::ios::out | std::ios::app);
    if (!out_) throw std::runtime_error("failed to open " + path.string());
    if (!exists && !is_hist) write_header_if_new(path);
}

void CsvWriter::write_header_if_new(const std::filesystem::path&) {
    auto it = PHASE_COLUMNS.find(phase_);
    if (it == PHASE_COLUMNS.end())
        throw std::runtime_error("unknown phase for CsvWriter: " + phase_);
    bool first = true;
    for (auto& c : kCommonColumns) {
        if (!first) out_ << ',';
        out_ << c;
        first = false;
    }
    for (auto& c : it->second) {
        out_ << ',' << c;
    }
    out_ << '\n';
    out_.flush();
}

void CsvWriter::write_raw_header(const std::vector<std::string>& columns) {
    // Only write if file is empty.
    out_.seekp(0, std::ios::end);
    if (out_.tellp() > 0) return;
    bool first = true;
    for (auto& c : columns) {
        if (!first) out_ << ',';
        out_ << c;
        first = false;
    }
    out_ << '\n';
    out_.flush();
}

void CsvWriter::write_raw_row(const std::vector<std::string>& values) {
    bool first = true;
    for (auto& v : values) {
        if (!first) out_ << ',';
        out_ << csv_escape(v);
        first = false;
    }
    out_ << '\n';
}

void CsvWriter::write_row(const std::vector<std::string>& values) {
    if (!common_prefix_) { write_raw_row(values); return; }
    auto it = PHASE_COLUMNS.find(phase_);
    if (it == PHASE_COLUMNS.end())
        throw std::runtime_error("unknown phase: " + phase_);
    if (values.size() != it->second.size())
        throw std::runtime_error("column count mismatch for phase " + phase_);

    out_ << csv_escape(meta_.run_id) << ','
         << csv_escape(meta_.timestamp_iso) << ','
         << csv_escape(meta_.sync_mode) << ','
         << meta_.writemap << ','
         << meta_.nordahead << ','
         << meta_.liforeclaim << ','
         << csv_escape(meta_.layout);
    for (auto& v : values) {
        out_ << ',' << csv_escape(v);
    }
    out_ << '\n';
}
