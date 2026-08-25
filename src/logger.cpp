#include "rtplot/logger.hpp"
#include "rtplot/shm_ipc.hpp"
#include "rtplot/storage.hpp"
#include "rtplot/udp.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace rtplot {
namespace {

uint64_t fnv1a(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

struct ChannelEntry {
  uint64_t nameHash = 0;
  std::string name;
  std::unique_ptr<RingBuffer<Sample>> ring;
  std::unique_ptr<RingBuffer<ArraySample>> arrayRing;
  std::atomic<uint64_t> pushed{0};
  std::atomic<uint64_t> dropped{0};
};

/// Fixed open-addressing channel registry. `find()` is lock-free: it only
/// loads std::atomic<ChannelEntry*> pointers. Creation happens under a mutex
/// and is therefore not on the real-time hot path.
class ChannelRegistry {
public:
  static constexpr size_t kSlots = 4096;

  ChannelRegistry() = default;

  ChannelEntry* find(std::string_view name) const {
    const uint64_t hash = fnv1a(name);
    size_t i = hash & (kSlots - 1);
    for (size_t visited = 0; visited < kSlots; ++visited, i = (i + 1) & (kSlots - 1)) {
      ChannelEntry* e = slots_[i].entry.load(std::memory_order_acquire);
      if (!e) return nullptr; // empty slot terminates the probe sequence
      if (e->nameHash == hash && e->name == name) return e;
    }
    return nullptr;
  }

  ChannelEntry* getOrCreate(std::string_view name, size_t ringCapacity) {
    if (ChannelEntry* e = find(name)) return e;
    std::lock_guard<std::mutex> lk(mutex_);
    if (ChannelEntry* e = find(name)) return e;

    auto entry = std::make_unique<ChannelEntry>();
    entry->nameHash = fnv1a(name);
    entry->name = std::string(name);
    entry->ring = std::make_unique<RingBuffer<Sample>>(ringCapacity);

    const uint64_t hash = entry->nameHash;
    size_t i = hash & (kSlots - 1);
    for (size_t visited = 0; visited < kSlots; ++visited, i = (i + 1) & (kSlots - 1)) {
      Slot& s = slots_[i];
      ChannelEntry* existing = s.entry.load(std::memory_order_acquire);
      if (existing) continue;
      // Publish pointer first, hash second. A concurrent lock-free lookup can
      // therefore resolve by pointer+name even before the hash is visible.
      s.entry.store(entry.get(), std::memory_order_release);
      s.nameHash.store(hash, std::memory_order_release);
      ChannelEntry* raw = entry.release();
      active_.push_back(raw);
      return raw;
    }
    return nullptr; // more than 4096 distinct channels
  }

  std::vector<ChannelEntry*> active() {
    std::lock_guard<std::mutex> lk(mutex_);
    return active_;
  }
  size_t size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return active_.size();
  }

private:
  struct alignas(64) Slot {
    std::atomic<uint64_t> nameHash{0};
    std::atomic<ChannelEntry*> entry{nullptr};
  };
  Slot slots_[kSlots];
  mutable std::mutex mutex_;
  std::vector<ChannelEntry*> active_;
};

} // namespace

class Logger::Impl {
public:
  ChannelRegistry registry;
  LoggerConfig cfg;
  std::atomic<bool> running{false};
  std::atomic<uint64_t> accepted{0};
  std::atomic<uint64_t> dropped{0};

  std::mutex startupMutex;
  std::mutex eventMutex;
  std::deque<Event> eventQueue;
  std::mutex flushMutex;
  std::condition_variable flushCv;
  std::atomic<bool> flushRequested{false};

  std::unique_ptr<StorageWriter> storage;
  std::unique_ptr<ShmPublisher> shm;
  std::unique_ptr<UdpPublisher> udp;
  std::thread worker;

  ~Impl() { stop(); }

  bool ensureStarted() {
    if (running.load(std::memory_order_acquire)) return true;
    std::lock_guard<std::mutex> lk(startupMutex);
    if (running.load(std::memory_order_acquire)) return true;
    return startLocked(LoggerConfig{});
  }

  bool start(const LoggerConfig& c) {
    std::lock_guard<std::mutex> lk(startupMutex);
    if (running.load(std::memory_order_acquire)) stopLocked();
    return startLocked(c);
  }

  bool startLocked(const LoggerConfig& c) {
    cfg = c;
    if (cfg.dropOldestWhenFull && cfg.overflowPolicy == OverflowPolicy::DropNewest) {
      cfg.overflowPolicy = OverflowPolicy::DropOldest; // live-display friendly
    }
    if (cfg.persist) {
      storage = std::make_unique<StorageWriter>();
      if (!storage->open(cfg.dbPath, StorageConfig{cfg.flushIntervalMs, 8u << 20})) return false;
    }
    if (cfg.shmPublish) {
      shm = std::make_unique<ShmPublisher>();
      if (!shm->start(cfg.shmName, cfg.shmRingCapacity)) return false;
    }
    if (cfg.udpPublish) {
      udp = std::make_unique<UdpPublisher>();
      if (!udp->start(cfg.udpAddress, cfg.udpPort)) return false;
    }
    accepted.store(0, std::memory_order_relaxed);
    dropped.store(0, std::memory_order_relaxed);
    running.store(true, std::memory_order_release);
    if (cfg.startBackgroundThread) {
      worker = std::thread([this] { run(); });
    }
    return true;
  }

  void stop() {
    std::lock_guard<std::mutex> lk(startupMutex);
    stopLocked();
  }

  void stopLocked() {
    if (!running.load(std::memory_order_acquire)) {
      if (worker.joinable()) worker.join();
      return;
    }
    running.store(false, std::memory_order_release);
    {
      flushRequested.store(true, std::memory_order_release);
    }
    flushCv.notify_all();
    if (worker.joinable()) worker.join();

    storage.reset();
    shm.reset();
    udp.reset();
    std::lock_guard<std::mutex> lk(eventMutex);
    eventQueue.clear();
  }

  void requestFlushAndWait() {
    if (!running.load(std::memory_order_acquire)) return;
    {
      flushRequested.store(true, std::memory_order_release);
    }
    flushCv.notify_all();
    std::unique_lock<std::mutex> lk(flushMutex);
    flushCv.wait_for(lk, std::chrono::milliseconds(1000), [this] {
      return !flushRequested.load(std::memory_order_acquire) ||
             !running.load(std::memory_order_acquire);
    });
  }

  void run() {
    std::vector<Sample> batch(1u << 16);
    std::vector<ArraySample> abatch(1u << 16);
    while (running.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lk(flushMutex);
        flushCv.wait_for(lk, std::chrono::milliseconds(cfg.flushIntervalMs));
      }
      drainOnce(batch, abatch);
      if (flushRequested.load(std::memory_order_acquire)) {
        finishFlush();
      }
    }
    drainOnce(batch, abatch); // final flush on stop
    finishFlush();
  }

  void finishFlush() {
    if (storage) storage->flush();
    flushRequested.store(false, std::memory_order_release);
    flushCv.notify_all();
  }

  void drainOnce(std::vector<Sample>& batch, std::vector<ArraySample>& abatch) {
    // Events (rare, not on the sample hot path).
    std::vector<Event> events;
    {
      std::lock_guard<std::mutex> lk(eventMutex);
      events.assign(eventQueue.begin(), eventQueue.end());
      eventQueue.clear();
    }
    for (const auto& ev : events) {
      if (storage) storage->writeEvent(ev);
      // Events are persisted in the .db file and rendered as timeline markers.
    }

    const auto active = registry.active();
    for (ChannelEntry* ch : active) {
      const size_t n = ch->ring->copyAvailable(batch.data(), batch.capacity());
      if (n) {
        ch->pushed.fetch_add(n, std::memory_order_relaxed);
        if (storage) storage->writeSamples(ch->name, batch.data(), n);
        if (shm) shm->publish(ch->name, batch.data(), n);
        if (udp) udp->publish(ch->name, batch.data(), n);
      }

      if (ch->arrayRing) {
        const size_t an = ch->arrayRing->copyAvailable(abatch.data(), abatch.size());
        if (an) {
          if (storage) storage->writeArraySamples(ch->name, abatch.data(), an);
          if (shm) shm->publishArray(ch->name, abatch.data(), an);
        }
      }
    }
  }

  bool log(std::string_view channel, double value, Timestamp t) {
    if (!ensureStarted()) return false;
    ChannelEntry* ch = registry.getOrCreate(channel, cfg.ringCapacity);
    if (!ch) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
    Sample s{t, value};
    const bool ok = ch->ring->write(s, cfg.overflowPolicy);
    if (ok) accepted.fetch_add(1, std::memory_order_relaxed);
    else {
      ch->dropped.fetch_add(1, std::memory_order_relaxed);
      dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
  }

  bool logArray(std::string_view channel, const double* values, size_t size, Timestamp t) {
    if (!values || size == 0 || size > kMaxArrayLength) return false;
    if (!ensureStarted()) return false;
    ChannelEntry* ch = registry.getOrCreate(channel, cfg.ringCapacity);
    if (!ch) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
    if (!ch->arrayRing) {
      // Lazily created on the first array sample; not on the scalar hot path.
      ch->arrayRing = std::make_unique<RingBuffer<ArraySample>>(cfg.ringCapacity);
    }
    ArraySample s;
    s.t = t;
    s.size = static_cast<uint32_t>(size);
    std::memcpy(s.values.data(), values, size * sizeof(double));
    const bool ok = ch->arrayRing->write(s, cfg.overflowPolicy);
    if (ok) accepted.fetch_add(1, std::memory_order_relaxed);
    else {
      ch->dropped.fetch_add(1, std::memory_order_relaxed);
      dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
  }

  bool event(std::string_view name, std::string_view payload, Timestamp t) {
    if (!ensureStarted()) return false;
    std::lock_guard<std::mutex> lk(eventMutex);
    if (eventQueue.size() >= 4096) return false;
    eventQueue.push_back(Event{t, std::string(name), std::string(payload)});
    return true;
  }

  size_t snapshotArray(std::string_view channel, std::vector<ArraySample>& out, size_t max) const {
    out.clear();
    ChannelEntry* ch = registry.find(channel);
    if (!ch || !ch->arrayRing) return 0;
    out.resize(max);
    const size_t n = ch->arrayRing->peek(out.data(), max);
    out.resize(n);
    return n;
  }

  size_t snapshot(std::string_view channel, std::vector<Sample>& out, size_t max) const {
    out.clear();
    ChannelEntry* ch = registry.find(channel);
    if (!ch) return 0;
    out.resize(max);
    const size_t n = ch->ring->peek(out.data(), max);
    out.resize(n);
    return n;
  }
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() { impl_->stop(); }

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

bool Logger::start(const LoggerConfig& cfg) { return impl_->start(cfg); }
void Logger::stop() { impl_->stop(); }
bool Logger::running() const noexcept { return impl_->running.load(std::memory_order_acquire); }
bool Logger::log(std::string_view channel, double value, Timestamp t) {
  return impl_->log(channel, value, t);
}
bool Logger::logArray(std::string_view channel, const double* values, size_t size, Timestamp t) {
  return impl_->logArray(channel, values, size, t);
}
bool Logger::event(std::string_view name, std::string_view payload, Timestamp t) {
  return impl_->event(name, payload, t);
}
void Logger::flushAndStop() {
  impl_->requestFlushAndWait();
  impl_->stop();
}
size_t Logger::snapshot(std::string_view channel, std::vector<Sample>& out, size_t max) const {
  return impl_->snapshot(channel, out, max);
}
size_t Logger::snapshotArray(std::string_view channel, std::vector<ArraySample>& out, size_t max) const {
  return impl_->snapshotArray(channel, out, max);
}
size_t Logger::channelCount() const { return impl_->registry.size(); }
const LoggerConfig& Logger::config() const noexcept { return impl_->cfg; }
uint64_t Logger::acceptedSamples() const noexcept {
  return impl_->accepted.load(std::memory_order_relaxed);
}
uint64_t Logger::droppedSamples() const noexcept {
  return impl_->dropped.load(std::memory_order_relaxed);
}

} // namespace rtplot
