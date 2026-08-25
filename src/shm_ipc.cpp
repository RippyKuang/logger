#include "rtplot/shm_ipc.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rtplot {
namespace {

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

struct alignas(64) ShmControlHeader {
  std::atomic<uint64_t> magic{0};
  std::atomic<uint64_t> version{0};
  std::atomic<uint64_t> publisherPid{0};
  uint64_t reserved[5] = {0, 0, 0, 0, 0};
};

struct alignas(64) ShmChannelSlot {
  std::atomic<uint64_t> nameHash{0};
  std::atomic<uint32_t> state{0};     // 0 empty, 1 active
  std::atomic<uint64_t> pushed{0};
  std::atomic<uint32_t> arrayLength{0};
  uint32_t reserved = 0;
  char name[kShmChannelNameLen] = {0};
  char segment[64] = {0};
};

struct ShmControl {
  ShmControlHeader header;
  ShmChannelSlot slots[kShmMaxChannels];
};

struct alignas(64) ShmDataHeader {
  std::atomic<uint64_t> magic{0};
  std::atomic<uint64_t> capacity{0};
  std::atomic<uint64_t> writePos{0};
  std::atomic<uint64_t> readPos{0};
  uint64_t reserved[4] = {0, 0, 0, 0};
};

struct DataMap {
  std::string segmentName;
  int fd = -1;
  void* map = MAP_FAILED;
  size_t mapSize = 0;
  ShmDataHeader* header = nullptr;
  Sample* slots = nullptr;
  ArraySample* arraySlots = nullptr;
  size_t capacity = 0;
  bool isArray = false;
};

size_t dataHeaderBytes() {
  return (sizeof(ShmDataHeader) + 63u) & ~size_t(63u);
}

std::string shmPath(const std::string& name) {
  return (!name.empty() && name[0] == '/') ? name : ("/" + name);
}

// Containers / sandboxes sometimes mount /dev/shm read-only or block creation
// (EACCES). The library then transparently falls back to a MAP_SHARED file in
// TMPDIR; the API and lock-free layout remain identical.
std::string fallbackPath(const std::string& name) {
  std::string clean;
  for (char c : name) clean += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
  return "/tmp/rtplot_shm_" + clean;
}

int openExistingShm(const std::string& name) {
  int fd = ::shm_open(shmPath(name).c_str(), O_RDWR, 0644);
  if (fd < 0) fd = ::open(fallbackPath(name).c_str(), O_RDWR);
  return fd;
}

void unlinkShm(const std::string& name) {
  ::shm_unlink(shmPath(name).c_str());
  ::unlink(fallbackPath(name).c_str());
}

bool openOrCreateShm(const std::string& name, size_t bytes, int& fd, bool& created) {
  int err = 0;
  fd = ::shm_open(shmPath(name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
  err = errno;
  if (fd < 0 && err == EACCES) {
    fd = ::open(fallbackPath(name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    err = errno;
  }
  if (fd >= 0) {
    created = true;
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) { ::close(fd); fd = -1; return false; }
    return true;
  }
  if (err != EEXIST) return false;
  created = false;
  fd = openExistingShm(name);
  if (fd < 0) {
    // A POSIX segment exists but cannot be opened (e.g. sandboxed /dev/shm);
    // create/open the file-backed fallback instead.
    fd = ::open(fallbackPath(name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
    created = fd >= 0;
    if (fd < 0 && errno == EEXIST) fd = ::open(fallbackPath(name).c_str(), O_RDWR);
    if (fd < 0) return false;
  }
  if (created && ::ftruncate(fd, static_cast<off_t>(bytes)) != 0) { ::close(fd); fd = -1; return false; }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(bytes)) { ::close(fd); fd = -1; return false; }
  return true;
}

void waitForMagic(const std::atomic<uint64_t>& magic, uint64_t expected) {
  for (int i = 0; i < 1000; ++i) {
    if (magic.load(std::memory_order_acquire) == expected) return;
    ::usleep(100);
  }
}

} // namespace

// ============================================================================
// ShmPublisher
// ============================================================================
class ShmPublisher::Impl {
public:
  std::string controlName;
  int controlFd = -1;
  void* controlMap = MAP_FAILED;
  ShmControl* control = nullptr;
  size_t defaultRingCapacity = 1u << 16;
  std::map<std::string, DataMap> segments;
  bool started = false;

  ~Impl() { stop(); }

  bool start(const std::string& ctrlName, size_t capacity) {
    stop();
    controlName = ctrlName;
    defaultRingCapacity = capacity;
    bool created = false;
    if (!openOrCreateShm(controlName, sizeof(ShmControl), controlFd, created)) return false;
    controlMap = ::mmap(nullptr, sizeof(ShmControl), PROT_READ | PROT_WRITE, MAP_SHARED, controlFd, 0);
    if (controlMap == MAP_FAILED) { ::close(controlFd); controlFd = -1; return false; }
    if (created) {
      control = new (controlMap) ShmControl();
      control->header.magic.store(kShmControlMagic, std::memory_order_release);
      control->header.version.store(1, std::memory_order_release);
      control->header.publisherPid.store(static_cast<uint64_t>(::getpid()), std::memory_order_release);
    } else {
      control = static_cast<ShmControl*>(controlMap);
      waitForMagic(control->header.magic, kShmControlMagic);
      if (control->header.magic.load(std::memory_order_acquire) != kShmControlMagic) {
        stop(); return false;
      }
    }
    started = true;
    return true;
  }

  void stop() {
    for (auto& kv : segments) {
      if (kv.second.map != MAP_FAILED) ::munmap(kv.second.map, kv.second.mapSize);
      if (kv.second.fd >= 0) ::close(kv.second.fd);
      unlinkShm(kv.second.segmentName);
    }
    segments.clear();
    if (controlMap != MAP_FAILED) ::munmap(controlMap, sizeof(ShmControl));
    controlMap = MAP_FAILED;
    control = nullptr;
    if (controlFd >= 0) ::close(controlFd);
    controlFd = -1;
    if (!controlName.empty()) unlinkShm(controlName);
    controlName.clear();
    started = false;
  }

  int findOrCreateSlot(const std::string& channel) {
    const uint64_t hash = fnv1a(channel);
    int empty = -1;
    for (size_t i = 0; i < kShmMaxChannels; ++i) {
      ShmChannelSlot& s = control->slots[i];
      const uint32_t state = s.state.load(std::memory_order_acquire);
      if (state == 1 && s.nameHash.load(std::memory_order_acquire) == hash &&
          channel == s.name) return static_cast<int>(i);
      if (state == 0 && empty < 0) empty = static_cast<int>(i);
    }
    if (empty < 0) return -1;
    ShmChannelSlot& s = control->slots[empty];
    std::memset(s.name, 0, sizeof(s.name));
    std::snprintf(s.segment, sizeof(s.segment), "rtplot_data_%d", empty);
    channel.copy(s.name, kShmChannelNameLen - 1);
    s.name[std::min(channel.size(), kShmChannelNameLen - 1)] = '\0';
    s.pushed.store(0, std::memory_order_relaxed);
    s.arrayLength.store(0, std::memory_order_release);
    s.nameHash.store(hash, std::memory_order_release);
    s.state.store(1, std::memory_order_release);
    return empty;
  }

  bool ensureSegment(const std::string& channel, int slot, bool isArray, DataMap*& out) {
    auto it = segments.find(channel + (isArray ? ":arr" : ":scalar"));
    if (it != segments.end()) { out = &it->second; return true; }

    DataMap dm;
    dm.isArray = isArray;
    dm.segmentName = std::string(isArray ? "rtplot_array_" : "rtplot_data_") + std::to_string(slot);
    dm.capacity = defaultRingCapacity;
    dm.mapSize = dataHeaderBytes() + dm.capacity * (isArray ? sizeof(ArraySample) : sizeof(Sample));
    bool created = false;
    if (!openOrCreateShm(dm.segmentName, dm.mapSize, dm.fd, created)) return false;
    dm.map = ::mmap(nullptr, dm.mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, dm.fd, 0);
    if (dm.map == MAP_FAILED) { ::close(dm.fd); return false; }
    if (created) {
      dm.header = new (dm.map) ShmDataHeader();
      dm.header->magic.store(kShmDataMagic, std::memory_order_release);
      dm.header->capacity.store(dm.capacity, std::memory_order_release);
    } else {
      dm.header = static_cast<ShmDataHeader*>(dm.map);
      waitForMagic(dm.header->magic, kShmDataMagic);
      if (dm.header->magic.load(std::memory_order_acquire) != kShmDataMagic) {
        ::munmap(dm.map, dm.mapSize); ::close(dm.fd); return false;
      }
      dm.capacity = static_cast<size_t>(dm.header->capacity.load(std::memory_order_acquire));
    }
    uint8_t* data = static_cast<uint8_t*>(dm.map) + dataHeaderBytes();
    if (isArray) dm.arraySlots = reinterpret_cast<ArraySample*>(data);
    else dm.slots = reinterpret_cast<Sample*>(data);
    auto res = segments.emplace(channel + (isArray ? ":arr" : ":scalar"), dm);
    out = &res.first->second;
    return true;
  }

  bool publish(const std::string& channel, const Sample* samples, size_t count) {
    if (!started || !samples || count == 0) return true;
    const int slot = findOrCreateSlot(channel);
    if (slot < 0) return false;
    DataMap* dm = nullptr;
    if (!ensureSegment(channel, slot, false, dm)) return false;

    ShmDataHeader* h = dm->header;
    uint64_t w = h->writePos.load(std::memory_order_relaxed);
    uint64_t r = h->readPos.load(std::memory_order_acquire);
    const uint64_t cap = dm->capacity;
    for (size_t i = 0; i < count; ++i) {
      // Live viewers prefer the newest data: drop oldest when they lag.
      while (w - r >= cap) {
        r = h->readPos.fetch_add(1, std::memory_order_acq_rel) + 1;
      }
      dm->slots[w % cap] = samples[i];
      w += 1;
    }
    h->writePos.store(w, std::memory_order_release);
    control->slots[slot].pushed.fetch_add(count, std::memory_order_release);
    return true;
  }

  bool publishArray(const std::string& channel, const ArraySample* samples, size_t count) {
    if (!started || !samples || count == 0) return true;
    const int slot = findOrCreateSlot(channel);
    if (slot < 0) return false;
    if (count) control->slots[slot].arrayLength.store(samples[0].size, std::memory_order_release);
    DataMap* dm = nullptr;
    if (!ensureSegment(channel, slot, true, dm)) return false;

    ShmDataHeader* h = dm->header;
    uint64_t w = h->writePos.load(std::memory_order_relaxed);
    uint64_t r = h->readPos.load(std::memory_order_acquire);
    const uint64_t cap = dm->capacity;
    for (size_t i = 0; i < count; ++i) {
      while (w - r >= cap) r = h->readPos.fetch_add(1, std::memory_order_acq_rel) + 1;
      dm->arraySlots[w % cap] = samples[i];
      w += 1;
    }
    h->writePos.store(w, std::memory_order_release);
    control->slots[slot].pushed.fetch_add(count, std::memory_order_release);
    return true;
  }
};

ShmPublisher::ShmPublisher() : impl_(std::make_unique<Impl>()) {}
ShmPublisher::~ShmPublisher() = default;
bool ShmPublisher::start(const std::string& controlName, size_t defaultRingCapacity) {
  return impl_->start(controlName, defaultRingCapacity);
}
void ShmPublisher::stop() { impl_->stop(); }
bool ShmPublisher::publish(const std::string& channel, const Sample* samples, size_t count) {
  return impl_->publish(channel, samples, count);
}
bool ShmPublisher::publishArray(const std::string& channel, const ArraySample* samples, size_t count) {
  return impl_->publishArray(channel, samples, count);
}

// ============================================================================
// ShmReader
// ============================================================================
class ShmReader::Impl {
public:
  std::string controlName;
  int controlFd = -1;
  void* controlMap = MAP_FAILED;
  ShmControl* control = nullptr;
  std::map<std::string, DataMap> segments;
  bool started = false;

  ~Impl() { stop(); }

  bool start(const std::string& ctrlName) {
    stop();
    controlName = ctrlName;
    controlFd = openExistingShm(controlName);
    if (controlFd < 0) return false;
    struct stat st{};
    if (::fstat(controlFd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ShmControl))) {
      ::close(controlFd); controlFd = -1; return false;
    }
    controlMap = ::mmap(nullptr, sizeof(ShmControl), PROT_READ | PROT_WRITE, MAP_SHARED, controlFd, 0);
    if (controlMap == MAP_FAILED) { ::close(controlFd); controlFd = -1; return false; }
    control = static_cast<ShmControl*>(controlMap);
    waitForMagic(control->header.magic, kShmControlMagic);
    if (control->header.magic.load(std::memory_order_acquire) != kShmControlMagic) { stop(); return false; }
    started = true;
    return true;
  }

  void stop() {
    for (auto& kv : segments) {
      if (kv.second.map != MAP_FAILED) ::munmap(kv.second.map, kv.second.mapSize);
      if (kv.second.fd >= 0) ::close(kv.second.fd);
    }
    segments.clear();
    if (controlMap != MAP_FAILED) ::munmap(controlMap, sizeof(ShmControl));
    controlMap = MAP_FAILED;
    control = nullptr;
    if (controlFd >= 0) ::close(controlFd);
    controlFd = -1;
    controlName.clear();
    started = false;
  }

  bool openSegment(int, const char* segmentName, bool isArray) {
    const std::string key = std::string(segmentName) + (isArray ? ":arr" : ":scalar");
    if (segments.count(key)) return true;
    DataMap dm;
    dm.segmentName = segmentName;
    dm.isArray = isArray;
    dm.fd = openExistingShm(segmentName);
    if (dm.fd < 0) return false;
    struct stat st{};
    if (::fstat(dm.fd, &st) != 0 ||
        st.st_size < static_cast<off_t>(dataHeaderBytes() + sizeof(Sample))) {
      ::close(dm.fd); return false;
    }
    dm.mapSize = static_cast<size_t>(st.st_size);
    dm.map = ::mmap(nullptr, dm.mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, dm.fd, 0);
    if (dm.map == MAP_FAILED) { ::close(dm.fd); return false; }
    dm.header = static_cast<ShmDataHeader*>(dm.map);
    waitForMagic(dm.header->magic, kShmDataMagic);
    if (dm.header->magic.load(std::memory_order_acquire) != kShmDataMagic) {
      ::munmap(dm.map, dm.mapSize); ::close(dm.fd); return false;
    }
    dm.capacity = static_cast<size_t>(dm.header->capacity.load(std::memory_order_acquire));
    const size_t slotSize = isArray ? sizeof(ArraySample) : sizeof(Sample);
    if (dm.capacity == 0 || dm.capacity > dm.mapSize / slotSize) {
      ::munmap(dm.map, dm.mapSize); ::close(dm.fd); return false;
    }
    uint8_t* data = static_cast<uint8_t*>(dm.map) + dataHeaderBytes();
    if (isArray) dm.arraySlots = reinterpret_cast<ArraySample*>(data);
    else dm.slots = reinterpret_cast<Sample*>(data);
    segments.emplace(key, dm);
    return true;
  }

  size_t copyArray(DataMap& dm, std::vector<ArraySample>& out, size_t maxSamples, bool advance) {
    ShmDataHeader* h = dm.header;
    const uint64_t r = h->readPos.load(std::memory_order_relaxed);
    const uint64_t w = h->writePos.load(std::memory_order_acquire);
    if (r >= w) return 0;
    const uint64_t avail = w - r;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(avail, maxSamples));
    out.resize(count);
    const uint64_t cap = dm.capacity;
    for (size_t i = 0; i < count; ++i) out[i] = dm.arraySlots[(r + i) % cap];
    if (advance) h->readPos.store(r + count, std::memory_order_release);
    return count;
  }

  size_t readArrayChannel(const std::string& channel, std::vector<ArraySample>& out, size_t maxSamples) {
    out.clear();
    if (!started) return 0;
    for (size_t i = 0; i < kShmMaxChannels; ++i) {
      ShmChannelSlot& s = control->slots[i];
      if (s.state.load(std::memory_order_acquire) != 1) continue;
      if (std::string(s.name, strnlen(s.name, kShmChannelNameLen)) != channel) continue;
      std::string seg(s.segment, strnlen(s.segment, sizeof(s.segment)));
      if (s.arrayLength.load(std::memory_order_acquire)) seg = "rtplot_array_" + std::to_string(i);
      auto it = segments.find(seg + ":arr");
      if (it == segments.end()) return 0;
      return copyArray(it->second, out, maxSamples, true);
    }
    return 0;
  }

  size_t copy(DataMap& dm, std::vector<Sample>& out, size_t maxSamples, bool advance) {
    ShmDataHeader* h = dm.header;
    const uint64_t r = h->readPos.load(std::memory_order_relaxed);
    const uint64_t w = h->writePos.load(std::memory_order_acquire);
    if (r >= w) return 0;
    const uint64_t avail = w - r;
    const size_t count = static_cast<size_t>(std::min<uint64_t>(avail, maxSamples));
    out.resize(count);
    const uint64_t cap = dm.capacity;
    for (size_t i = 0; i < count; ++i) out[i] = dm.slots[(r + i) % cap];
    if (advance) h->readPos.store(r + count, std::memory_order_release);
    return count;
  }
};

ShmReader::ShmReader() : impl_(std::make_unique<Impl>()) {}
ShmReader::~ShmReader() = default;
bool ShmReader::start(const std::string& controlName) { return impl_->start(controlName); }
void ShmReader::stop() { impl_->stop(); }
bool ShmReader::isOpen() const noexcept { return impl_ && impl_->started; }

std::vector<ShmReader::Channel> ShmReader::discover() {
  std::vector<Channel> out;
  if (!impl_->started) return out;
  for (size_t i = 0; i < kShmMaxChannels; ++i) {
    ShmChannelSlot& s = impl_->control->slots[i];
    if (s.state.load(std::memory_order_acquire) != 1) continue;
    std::string name(s.name, strnlen(s.name, kShmChannelNameLen));
    std::string seg(s.segment, strnlen(s.segment, sizeof(s.segment)));
    if (name.empty() || seg.empty()) continue;
    const uint32_t arrayLen = s.arrayLength.load(std::memory_order_acquire);
    if (arrayLen) seg = "rtplot_array_" + std::to_string(i);
    if (!impl_->openSegment(static_cast<int>(i), seg.c_str(), arrayLen != 0)) continue;
    const std::string key = seg + (arrayLen ? ":arr" : ":scalar");
    auto it = impl_->segments.find(key);
    Channel c;
    c.name = name;
    c.slot = static_cast<uint32_t>(i);
    c.segment = seg;
    c.capacity = it != impl_->segments.end() ? it->second.capacity : 0;
    c.pushed = s.pushed.load(std::memory_order_acquire);
    c.isArray = arrayLen != 0;
    c.arrayLength = arrayLen;
    out.push_back(std::move(c));
  }
  return out;
}

size_t ShmReader::read(const std::string& channel, std::vector<Sample>& out, size_t maxSamples) {
  out.clear();
  if (!impl_->started) return 0;
  for (size_t i = 0; i < kShmMaxChannels; ++i) {
    ShmChannelSlot& s = impl_->control->slots[i];
    if (s.state.load(std::memory_order_acquire) != 1) continue;
    if (std::string(s.name, strnlen(s.name, kShmChannelNameLen)) != channel) continue;
    auto it = impl_->segments.find(std::string(s.segment, strnlen(s.segment, sizeof(s.segment))) + ":scalar");
    if (it == impl_->segments.end()) return 0;
    return impl_->copy(it->second, out, maxSamples, true);
  }
  return 0;
}

size_t ShmReader::readArray(const std::string& channel, std::vector<ArraySample>& out, size_t maxSamples) {
  return impl_->readArrayChannel(channel, out, maxSamples);
}

size_t ShmReader::peek(const std::string& channel, std::vector<Sample>& out, size_t maxSamples) {
  out.clear();
  if (!impl_->started) return 0;
  for (size_t i = 0; i < kShmMaxChannels; ++i) {
    ShmChannelSlot& s = impl_->control->slots[i];
    if (s.state.load(std::memory_order_acquire) != 1) continue;
    if (std::string(s.name, strnlen(s.name, kShmChannelNameLen)) != channel) continue;
    auto it = impl_->segments.find(std::string(s.segment, strnlen(s.segment, sizeof(s.segment))) + ":scalar");
    if (it == impl_->segments.end()) return 0;
    return impl_->copy(it->second, out, maxSamples, false);
  }
  return 0;
}

} // namespace rtplot
