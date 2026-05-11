#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct RunMeta {
    std::string run_id;
    std::string timestamp_iso;
    std::string sync_mode;
    int         writemap    = 0;
    int         nordahead   = 0;
    int         liforeclaim = 0;
    std::string layout      = "single"; // "single" | "per_dbi"
};

extern const std::unordered_map<std::string, std::vector<std::string>> PHASE_COLUMNS;

class CsvWriter {
public:
    CsvWriter(const std::filesystem::path& out_dir,
              std::string phase,
              const RunMeta& meta);

    // Phase rows: prepended with RunMeta common columns.
    void write_row(const std::vector<std::string>& values);

    // Raw rows (used for histogram dump files with custom schema).
    // No common-column prefix. Caller must write a matching header first.
    void write_raw_header(const std::vector<std::string>& columns);
    void write_raw_row(const std::vector<std::string>& values);

private:
    std::ofstream out_;
    std::string   phase_;
    RunMeta       meta_;
    bool          common_prefix_ = true;
    void write_header_if_new(const std::filesystem::path& path);
};

std::string iso8601_utc_now();
std::string csv_escape(std::string_view s);
