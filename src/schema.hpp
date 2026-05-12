#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "mdbx.h++"

struct DBISpec
{
    const char *name;
    size_t key_size;
    size_t val_size;
    unsigned flags;
    size_t record_count;
};

inline constexpr DBISpec SCHEMA[] = {
    {"vectors", 4, 1540, MDBX_INTEGERKEY, 10'000'000},
    {"meta", 4, 75, 0, 10'000'000},
    {"id", 7, 4, 0, 10'000'000},
    {"filter_schema", 17, 48, 0, 5},
    {"filter_numeric_inverted", 7, 10'000, 0, 14000},
    {"filter_numeric_forward", 10, 4, 0, 10'000'000},
    {"filter_category", 16, 2, 0, 10}
};
inline constexpr size_t DBI_COUNT = sizeof(SCHEMA) / sizeof(SCHEMA[0]);

// Writes `seq` into `buf` in big-endian order, right-aligned to `key_size`.
// If key_size > 8 the high bytes are zero-padded; if key_size < 8 the
// high bytes of `seq` that don't fit are dropped (so callers must keep
// `seq < 2^(8*key_size)` for unique keys). This preserves lexicographic
// order so MDBX_APPEND works for monotonically increasing seq.
inline void make_key_seq(uint8_t *buf, size_t key_size, uint64_t seq)
{
    std::memset(buf, 0, key_size);
    size_t n = std::min(sizeof(seq), key_size);
    for (size_t i = 0; i < n; ++i)
    {
        buf[key_size - 1 - i] = static_cast<uint8_t>(seq >> (8 * i));
    }
}

inline void make_key_rand(uint8_t *buf, size_t key_size, uint64_t max_key, std::mt19937_64 &rng)
{
    uint64_t k = rng() % max_key;
    make_key_seq(buf, key_size, k);
}

// Zipfian generator: build CDF once, sample with binary search.
class ZipfGen
{
    std::vector<double> cdf_;
    uint64_t max_key_ = 0;

public:
    ZipfGen() = default;
    ZipfGen(uint64_t max_key, double theta) { init(max_key, theta); }

    void init(uint64_t max_key, double theta)
    {
        max_key_ = max_key;
        cdf_.assign(max_key, 0.0);
        double zeta = 0.0;
        for (uint64_t i = 1; i <= max_key; ++i)
        {
            zeta += 1.0 / std::pow(static_cast<double>(i), theta);
            cdf_[i - 1] = zeta;
        }
        for (auto &c : cdf_)
            c /= zeta;
    }

    uint64_t sample(std::mt19937_64 &rng) const
    {
        double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
        auto it = std::lower_bound(cdf_.begin(), cdf_.end(), u);
        return static_cast<uint64_t>(std::distance(cdf_.begin(), it));
    }

    uint64_t max_key() const { return max_key_; }
};

inline void make_val(uint8_t *buf, size_t val_size, uint64_t key_seq)
{
    std::memset(buf, static_cast<uint8_t>(key_seq & 0xFF), val_size);
    std::memcpy(buf, &key_seq, std::min(sizeof(key_seq), val_size));
}
