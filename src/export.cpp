#include "rtplot/storage.hpp"

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace rtplot {

bool exportCsv(const std::string& dbPath, const std::string& csvPath, char delimiter) {
  StorageReader reader;
  if (!reader.open(dbPath)) return false;

  std::vector<ChannelInfo> scalarChannels;
  for (const auto& c : reader.channels()) {
    if (!c.isArray) scalarChannels.push_back(c);
  }
  if (scalarChannels.empty()) return false;

  std::vector<std::vector<Sample>> series;
  series.reserve(scalarChannels.size());
  Timestamp origin = INT64_MAX;
  for (const auto& c : scalarChannels) {
    series.push_back(reader.readSamples(c.name));
    if (!series.back().empty()) origin = std::min(origin, series.back().front().t);
  }
  if (origin == INT64_MAX) origin = 0;

  FILE* f = std::fopen(csvPath.c_str(), "wb");
  if (!f) return false;

  std::string header = "t_ns,t_rel_s";
  for (const auto& c : scalarChannels) {
    header.push_back(delimiter);
    header += c.name;
  }
  header += '\n';
  std::fwrite(header.data(), 1, header.size(), f);

  std::vector<size_t> idx(series.size(), 0);
  std::vector<double> last(series.size(), 0.0);
  std::vector<bool> has(series.size(), false);

  auto pickOldest = [&]() -> int {
    int best = -1;
    Timestamp bt = 0;
    for (size_t i = 0; i < series.size(); ++i) {
      if (idx[i] >= series[i].size()) continue;
      const Timestamp t = series[i][idx[i]].t;
      if (best < 0 || t < bt) { best = static_cast<int>(i); bt = t; }
    }
    return best;
  };

  char buf[80];
  while (true) {
    const int bi = pickOldest();
    if (bi < 0) break;
    const Timestamp t = series[static_cast<size_t>(bi)][idx[static_cast<size_t>(bi)]].t;
    for (size_t i = 0; i < series.size(); ++i) {
      while (idx[i] < series[i].size() && series[i][idx[i]].t == t) {
        last[i] = series[i][idx[i]].v;
        has[i] = true;
        ++idx[i];
      }
    }
    int n = std::snprintf(buf, sizeof(buf), "%lld,%.9f", static_cast<long long>(t),
                          static_cast<double>(t - origin) * 1e-9);
    std::fwrite(buf, 1, static_cast<size_t>(n), f);
    for (size_t i = 0; i < series.size(); ++i) {
      n = std::snprintf(buf, sizeof(buf), "%c%.17g", delimiter,
                        has[i] ? last[i] : std::numeric_limits<double>::quiet_NaN());
      std::fwrite(buf, 1, static_cast<size_t>(n), f);
    }
    std::fwrite("\n", 1, 1, f);
  }
  std::fclose(f);
  return true;
}

} // namespace rtplot
