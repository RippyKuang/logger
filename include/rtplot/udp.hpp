#pragma once
#include "rtplot/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtplot {

struct UdpPacketHeader {
  uint32_t magic = 0x52545055u; // 'RTPU'
  uint32_t sequence = 0;
  uint32_t channelNameLen = 0;
  uint32_t sampleCount = 0;
  uint64_t firstTs = 0;
};

/// Minimal binary UDP telemetry push. Packet = header + channel name +
/// (int64 timestamp, double value) pairs. Little-endian fields are encoded
/// explicitly, so the recorder and remote viewer can run on any CPU.
class UdpPublisher {
public:
  UdpPublisher();
  ~UdpPublisher();
  UdpPublisher(const UdpPublisher&) = delete;
  UdpPublisher& operator=(const UdpPublisher&) = delete;

  bool start(const std::string& address = "127.0.0.1", uint16_t port = 9870);
  void stop();
  [[nodiscard]] bool isOpen() const noexcept;

  /// Send one batch. Returns false on socket error (e.g. would block on a full
  /// OS send buffer).
  bool publish(const std::string& channel, const Sample* samples, size_t count);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class UdpReceiver {
public:
  UdpReceiver();
  ~UdpReceiver();
  UdpReceiver(const UdpReceiver&) = delete;
  UdpReceiver& operator=(const UdpReceiver&) = delete;

  /// Bind to 0.0.0.0:port. Set reuseAddress to share the port.
  bool start(uint16_t port = 9870, bool reuseAddress = true);
  void stop();
  [[nodiscard]] bool isOpen() const noexcept;

  /// Non-blocking receive of one datagram. Returns false when no datagram is
  /// pending or the datagram is malformed.
  bool receive(std::string& channel, std::vector<Sample>& out);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace rtplot
