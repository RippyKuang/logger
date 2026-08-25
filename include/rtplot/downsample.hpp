#pragma once
#include "rtplot/types.hpp"
#include <algorithm>
#include <cassert>

namespace rtplot {

/// Per-pixel MinMax downsampling. Produces at most `maxPoints` points while
/// preserving local extrema. Time complexity O(n).
inline std::vector<Sample> downsampleMinMax(const std::vector<Sample>& in,
                                            size_t maxPoints) {
  std::vector<Sample> out;
  const size_t n = in.size();
  if (maxPoints < 2 || n <= maxPoints) { out = in; return out; }

  // Each bucket emits min and max, therefore the requested number is halved.
  const size_t bucketTarget = std::max<size_t>(1, maxPoints / 2);
  const size_t bucketSize = std::max<size_t>(1, n / bucketTarget);
  const size_t bucketCount = (n + bucketSize - 1) / bucketSize;
  out.reserve(bucketCount * 2 + 2);

  for (size_t b = 0; b < bucketCount; ++b) {
    const size_t begin = b * bucketSize;
    const size_t end = std::min(n, begin + bucketSize);
    size_t imin = begin, imax = begin;
    for (size_t i = begin + 1; i < end; ++i) {
      if (in[i].v < in[imin].v) imin = i;
      if (in[i].v > in[imax].v) imax = i;
    }
    if (imin == imax) {
      out.push_back(in[imin]);
    } else {
      if (imin < imax) { out.push_back(in[imin]); out.push_back(in[imax]); }
      else             { out.push_back(in[imax]); out.push_back(in[imin]); }
    }
  }
  return out;
}

/// Largest-Triangle-Three-Buckets downsampling (Steinarsson). O(n).
inline std::vector<Sample> downsampleLTTB(const std::vector<Sample>& in,
                                          size_t maxPoints) {
  const size_t n = in.size();
  if (maxPoints < 3 || n <= maxPoints) return in;
  std::vector<Sample> out;
  out.reserve(maxPoints);

  const double every = static_cast<double>(n - 2) / static_cast<double>(maxPoints - 2);
  size_t a = 0;
  out.push_back(in[a]);

  for (size_t i = 0; i < maxPoints - 2; ++i) {
    const size_t avgStart = static_cast<size_t>((i + 1) * every) + 1;
    const size_t avgEnd = std::min(n, static_cast<size_t>((i + 2) * every) + 1);
    const size_t avgRange = std::max<size_t>(1, avgEnd - avgStart);

    double avgX = 0.0, avgY = 0.0;
    for (size_t j = avgStart; j < avgEnd; ++j) {
      avgX += static_cast<double>(in[j].t);
      avgY += in[j].v;
    }
    avgX /= static_cast<double>(avgRange);
    avgY /= static_cast<double>(avgRange);

    const size_t rangeOff = static_cast<size_t>(i * every) + 1;
    const size_t rangeTo = std::min(n, static_cast<size_t>((i + 1) * every) + 1);
    const double ax = static_cast<double>(in[a].t);
    const double ay = in[a].v;

    size_t chosen = rangeOff;
    double maxArea = -1.0;
    for (size_t j = rangeOff; j < rangeTo; ++j) {
      const double area = std::fabs((ax - avgX) * (in[j].v - ay) -
                                    (ax - static_cast<double>(in[j].t)) * (avgY - ay));
      if (area > maxArea) { maxArea = area; chosen = j; }
    }
    out.push_back(in[chosen]);
    a = chosen;
  }
  out.push_back(in[n - 1]);
  return out;
}

inline std::vector<Sample> downsample(const std::vector<Sample>& in,
                                      size_t maxPoints,
                                      DownsampleAlgorithm algo = DownsampleAlgorithm::MinMax) {
  switch (algo) {
    case DownsampleAlgorithm::LTTB: return downsampleLTTB(in, maxPoints);
    case DownsampleAlgorithm::Decimate: {
      if (maxPoints == 0 || in.size() <= maxPoints) return in;
      std::vector<Sample> out;
      out.reserve(maxPoints);
      const double step = static_cast<double>(in.size()) / static_cast<double>(maxPoints);
      for (size_t i = 0; i < maxPoints; ++i) {
        out.push_back(in[static_cast<size_t>(i * step)]);
      }
      return out;
    }
    case DownsampleAlgorithm::MinMax:
    default: return downsampleMinMax(in, maxPoints);
  }
}

} // namespace rtplot
