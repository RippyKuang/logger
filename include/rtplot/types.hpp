#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <chrono>

namespace rtplot {

using Timestamp = int64_t;  ///< nanoseconds since epoch (UTC)

struct Sample {
  Timestamp t{0};
  double v{0.0};

  bool operator==(const Sample& o) const noexcept { return t == o.t && v == o.v; }
};

inline constexpr size_t kMaxArrayLength = 16;

/// Fixed-capacity array channel sample, used for positions (3), Euler angles
/// (3), quaternions (4), twists, etc. The fixed inline storage keeps the
/// front-end SPSC ring wait-free (no per-sample heap allocation).
struct ArraySample {
  Timestamp t{0};
  uint32_t size = 0;
  std::array<double, kMaxArrayLength> values{};

  [[nodiscard]] double operator[](size_t i) const noexcept { return values[i]; }
};

struct Event {
  Timestamp t{0};
  std::string name;
  std::string payload;
};

struct ChannelInfo {
  uint32_t id{0};
  std::string name;
  uint64_t sampleCount{0};
  Timestamp firstTs{0};
  Timestamp lastTs{0};
  bool isArray = false;
  uint32_t arrayLength = 0;
};

struct Stats {
  uint64_t count{0};
  double min{std::numeric_limits<double>::infinity()};
  double max{-std::numeric_limits<double>::infinity()};
  double mean{0.0};
  double variance{0.0};
  double stddev{0.0};
  double rms{0.0};

  [[nodiscard]] double span() const noexcept {
    return (count >= 2) ? (max - min) : 0.0;
  }
};

inline Stats computeStats(const Sample* s, size_t n) noexcept {
  Stats out;
  if (!s || n == 0) { out.count = 0; return out; }
  out.count = n;
  double sum = 0.0, sum2 = 0.0;
  out.min = std::numeric_limits<double>::infinity();
  out.max = -std::numeric_limits<double>::infinity();
  for (size_t i = 0; i < n; ++i) {
    const double v = s[i].v;
    out.min = std::min(out.min, v);
    out.max = std::max(out.max, v);
    sum += v;
    sum2 += v * v;
  }
  out.mean = sum / static_cast<double>(n);
  const double var = (n > 1) ? (sum2 - sum * out.mean) / static_cast<double>(n - 1)
                             : 0.0;
  out.variance = var > 0.0 ? var : 0.0;
  out.stddev = std::sqrt(out.variance);
  out.rms = std::sqrt(sum2 / static_cast<double>(n));
  return out;
}

inline Stats computeStats(const std::vector<Sample>& v) noexcept {
  return computeStats(v.data(), v.size());
}

inline Timestamp nowNs() noexcept {
  return static_cast<Timestamp>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

enum class DownsampleAlgorithm {
  MinMax,   ///< per-pixel min/max envelope (two points per bucket)
  LTTB,     ///< Largest-Triangle-Three-Buckets, visually faithful
  Decimate, ///< every Nth point
};

constexpr const char* to_string(DownsampleAlgorithm a) noexcept {
  return a == DownsampleAlgorithm::MinMax ? "MinMax" :
         a == DownsampleAlgorithm::LTTB ? "LTTB" : "Decimate";
}

} // namespace rtplot
