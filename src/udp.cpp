#include "rtplot/udp.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rtplot {
namespace {

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void appendU64(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
uint32_t peekU32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
         (uint32_t(p[3]) << 24);
}

bool setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

// ============================================================================
// UdpPublisher
// ============================================================================
class UdpPublisher::Impl {
public:
  int fd = -1;
  uint32_t sequence = 0;
  bool started = false;

  ~Impl() { stop(); }

  bool start(const std::string& address, uint16_t port) {
    stop();
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    setNonBlocking(fd);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) { stop(); return false; }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { stop(); return false; }
    started = true;
    return true;
  }

  void stop() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    started = false;
    sequence = 0;
  }

  bool publish(const std::string& channel, const Sample* samples, size_t count) {
    if (!started || !samples || count == 0 || channel.size() > 0xFFFF) return true;
    // Keep one UDP datagram well below the 64 KiB IP payload limit; large
    // batches are split into multiple sequenced datagrams.
    constexpr size_t kMaxSamplesPerDatagram = 800;
    size_t offset = 0;
    while (offset < count) {
      const size_t n = std::min(count - offset, kMaxSamplesPerDatagram);
      std::vector<uint8_t> pkt;
      pkt.reserve(sizeof(UdpPacketHeader) + channel.size() + n * sizeof(Sample));
      appendU32(pkt, 0x52545055u);
      appendU32(pkt, sequence++);
      appendU32(pkt, static_cast<uint32_t>(channel.size()));
      appendU32(pkt, static_cast<uint32_t>(n));
      appendU64(pkt, static_cast<uint64_t>(samples[offset].t));
      pkt.insert(pkt.end(), channel.begin(), channel.end());
      const uint8_t* sp = reinterpret_cast<const uint8_t*>(samples + offset);
      pkt.insert(pkt.end(), sp, sp + n * sizeof(Sample));
      const ssize_t sent = ::send(fd, pkt.data(), pkt.size(), 0);
      if (sent != static_cast<ssize_t>(pkt.size())) {
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
        if (sent < 0) return false;
      }
      offset += n;
    }
    return true;
  }
};

UdpPublisher::UdpPublisher() : impl_(std::make_unique<Impl>()) {}
UdpPublisher::~UdpPublisher() = default;
bool UdpPublisher::start(const std::string& address, uint16_t port) {
  return impl_->start(address, port);
}
void UdpPublisher::stop() { impl_->stop(); }
bool UdpPublisher::isOpen() const noexcept { return impl_ && impl_->started; }
bool UdpPublisher::publish(const std::string& channel, const Sample* samples, size_t count) {
  return impl_->publish(channel, samples, count);
}

// ============================================================================
// UdpReceiver
// ============================================================================
class UdpReceiver::Impl {
public:
  int fd = -1;
  bool started = false;

  ~Impl() { stop(); }

  bool start(uint16_t port, bool reuseAddress) {
    stop();
    fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    if (reuseAddress) {
      int one = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { stop(); return false; }
    setNonBlocking(fd);
    started = true;
    return true;
  }

  void stop() {
    if (fd >= 0) ::close(fd);
    fd = -1;
    started = false;
  }

  bool receive(std::string& channel, std::vector<Sample>& out) {
    if (!started) return false;
    uint8_t buf[65536];
    sockaddr_storage from{};
    socklen_t fromLen = sizeof(from);
    const ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n < 0) return false;
    if (n < static_cast<ssize_t>(sizeof(UdpPacketHeader))) return false;
    if (peekU32(buf) != 0x52545055u) return false;
    const uint32_t nameLen = peekU32(buf + 8);
    const uint32_t sampleCount = peekU32(buf + 12);
    const uint64_t expected = sizeof(UdpPacketHeader) + size_t(nameLen) +
                              size_t(sampleCount) * sizeof(Sample);
    if (n != static_cast<ssize_t>(expected) || nameLen > 4096) return false;
    channel.assign(reinterpret_cast<const char*>(buf + sizeof(UdpPacketHeader)), nameLen);
    out.resize(sampleCount);
    std::memcpy(out.data(), buf + sizeof(UdpPacketHeader) + nameLen,
                sampleCount * sizeof(Sample));
    return true;
  }
};

UdpReceiver::UdpReceiver() : impl_(std::make_unique<Impl>()) {}
UdpReceiver::~UdpReceiver() = default;
bool UdpReceiver::start(uint16_t port, bool reuseAddress) { return impl_->start(port, reuseAddress); }
void UdpReceiver::stop() { impl_->stop(); }
bool UdpReceiver::isOpen() const noexcept { return impl_ && impl_->started; }
bool UdpReceiver::receive(std::string& channel, std::vector<Sample>& out) {
  return impl_->receive(channel, out);
}

} // namespace rtplot
