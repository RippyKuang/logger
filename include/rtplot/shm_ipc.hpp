#pragma once
#include "rtplot/types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtplot {

inline constexpr uint32_t kShmControlMagic = 0x52545043u; // 'RTPC'
inline constexpr uint32_t kShmDataMagic = 0x52545044u;    // 'RTPD'
inline constexpr size_t kShmMaxChannels = 64;
inline constexpr size_t kShmChannelNameLen = 120;

/// Shared-memory IPC architecture:
///
///   recorder process                     viewer process
///  ┌──────────────────┐  POSIX shm   ┌──────────────────┐
///  │ Logger bg thread │─────────────▶│ ShmReader (SPSC) │
///  │ ShmPublisher     │  rtplot_ctrl │   per-channel     │
///  └──────────────────┘  + ch. rings └──────────────────┘
///
/// The control segment contains a fixed table of channel descriptors. Each
/// channel owns its own SPSC data segment named rtplot_data_<slot>; samples are
/// written lock-free by the (single) publisher and read by a (single) viewer.
class ShmPublisher {
public:
  ShmPublisher();
  ~ShmPublisher();
  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;

  bool start(const std::string& controlName = "rtplot_ctrl",
             size_t defaultRingCapacity = 1u << 16);
  void stop();

  /// Publish a batch. Not internally thread-safe; called from the logger's
  /// dedicated writer thread.
  bool publish(const std::string& channel, const Sample* samples, size_t count);
  bool publishArray(const std::string& channel, const ArraySample* samples, size_t count);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class ShmReader {
public:
  struct Channel {
    std::string name;
    uint32_t slot = 0;
    std::string segment;
    size_t capacity = 0;
    uint64_t pushed = 0;
    bool isArray = false;
    uint32_t arrayLength = 0;
  };

  ShmReader();
  ~ShmReader();
  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;

  bool start(const std::string& controlName = "rtplot_ctrl");
  void stop();

  /// Re-read the control table and open newly discovered channel segments.
  std::vector<Channel> discover();

  /// Consume available samples from one channel (single reader thread).
  size_t read(const std::string& channel, std::vector<Sample>& out,
              size_t maxSamples = 1u << 16);
  size_t peek(const std::string& channel, std::vector<Sample>& out,
              size_t maxSamples = 1u << 16);
  size_t readArray(const std::string& channel, std::vector<ArraySample>& out,
                   size_t maxSamples = 1u << 16);

  [[nodiscard]] bool isOpen() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rtplot
