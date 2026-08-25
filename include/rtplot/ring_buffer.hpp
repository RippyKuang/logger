#pragma once
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <immintrin.h>
#define RTPLOT_SPIN_PAUSE() _mm_pause()
#else
#define RTPLOT_SPIN_PAUSE() ((void)0)
#endif

namespace rtplot {

enum class OverflowPolicy {
  DropNewest,  ///< never blocks; newest sample is discarded while full
  DropOldest,  ///< never blocks; oldest unread sample is discarded
  Spin,        ///< busy-wait until a slot is free (bounded by reader progress)
};

/// Cache-line padded 64-bit atomic used to avoid false sharing between the
/// producer and consumer side of the queue.
struct alignas(64) PaddedAtomicU64 {
  std::atomic<uint64_t> value{0};
  uint8_t pad[64 - sizeof(std::atomic<uint64_t>)];
};

/// Bounded single-producer / single-consumer (SPSC) FIFO.
///
/// The capacity is rounded up to a power of two. `tryWrite`/`tryRead` are
/// wait-free and only use release/acquire ordered atomics; the common path has
/// no CAS, no mutex and no memory allocation. This is the front-end hot path
/// of the logger and normally costs 10-30 ns per push.
template <typename T>
class RingBuffer {
public:
  RingBuffer() : RingBuffer(1024) {}
  explicit RingBuffer(size_t minCapacity) {
    capacity_ = nextPow2(minCapacity < 2 ? 2 : minCapacity);
    mask_ = capacity_ - 1;
    slots_.resize(capacity_);
    writePos_.store(0, std::memory_order_relaxed);
    readPos_.store(0, std::memory_order_relaxed);
  }
  ~RingBuffer() = default;

  RingBuffer(const RingBuffer&) = delete;
  RingBuffer& operator=(const RingBuffer&) = delete;
  RingBuffer(RingBuffer&&) = delete;
  RingBuffer& operator=(RingBuffer&&) = delete;

  [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] size_t size() const noexcept {
    const uint64_t w = writePos_.load(std::memory_order_acquire);
    const uint64_t r = readPos_.load(std::memory_order_acquire);
    return static_cast<size_t>(w >= r ? w - r : 0);
  }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] bool full() const noexcept { return size() >= capacity_; }

  /// Producer API (one producer thread only).
  bool tryWrite(const T& item) noexcept {
    const uint64_t w = writePos_.load(std::memory_order_relaxed);
    const uint64_t r = readPos_.load(std::memory_order_acquire);
    if (w - r >= capacity_) return false;
    slots_[static_cast<size_t>(w & mask_)] = item;
    writePos_.store(w + 1, std::memory_order_release);
    return true;
  }

  /// Producer API. DropOldest deliberately advances the consumer cursor by
  /// one, which skips the oldest sample but keeps the latest data visible.
  /// This is the desired behaviour for live telemetry displays.
  bool write(const T& item, OverflowPolicy policy = OverflowPolicy::DropNewest) noexcept {
    switch (policy) {
      case OverflowPolicy::Spin: {
        while (!tryWrite(item)) RTPLOT_SPIN_PAUSE();
        return true;
      }
      case OverflowPolicy::DropOldest: {
        while (true) {
          if (tryWrite(item)) return true;
          // Free one slot; if a consumer advances concurrently an extra old
          // sample may be skipped, which is still "drop oldest" semantics.
          readPos_.fetch_add(1, std::memory_order_acq_rel);
        }
      }
      case OverflowPolicy::DropNewest:
      default:
        return tryWrite(item);
    }
  }

  /// Consumer API (one consumer thread only).
  bool tryRead(T& item) noexcept {
    const uint64_t r = readPos_.load(std::memory_order_relaxed);
    const uint64_t w = writePos_.load(std::memory_order_acquire);
    if (r >= w) return false;
    item = slots_[static_cast<size_t>(r & mask_)];
    readPos_.store(r + 1, std::memory_order_release);
    return true;
  }

  /// Copy at most `maxItems` available samples to `dst`.
  /// Returns the number of copied items. Wait-free for the consumer.
  size_t copyAvailable(T* dst, size_t maxItems) noexcept {
    const uint64_t r = readPos_.load(std::memory_order_relaxed);
    const uint64_t w = writePos_.load(std::memory_order_acquire);
    if (r >= w || !dst || maxItems == 0) return 0;
    const size_t avail = static_cast<size_t>(w - r);
    const size_t count = avail < maxItems ? avail : maxItems;
    for (size_t i = 0; i < count; ++i) {
      dst[i] = slots_[static_cast<size_t>((r + i) & mask_)];
    }
    readPos_.store(r + count, std::memory_order_release);
    return count;
  }

  /// Peek up to `maxItems` without advancing the consumer cursor.
  size_t peek(T* dst, size_t maxItems) const noexcept {
    const uint64_t r = readPos_.load(std::memory_order_relaxed);
    const uint64_t w = writePos_.load(std::memory_order_acquire);
    if (r >= w || !dst || maxItems == 0) return 0;
    const size_t count = std::min<size_t>(static_cast<size_t>(w - r), maxItems);
    for (size_t i = 0; i < count; ++i) {
      dst[i] = slots_[static_cast<size_t>((r + i) & mask_)];
    }
    return count;
  }

  /// Consumer-side "read position" in shared memory segments. Mostly useful
  /// for publishers that need to check how far the single consumer is.
  void setReadPosition(uint64_t pos) noexcept {
    readPos_.store(pos, std::memory_order_release);
  }
  [[nodiscard]] uint64_t readPosition() const noexcept {
    return readPos_.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint64_t writePosition() const noexcept {
    return writePos_.load(std::memory_order_acquire);
  }

private:
  static size_t nextPow2(size_t v) noexcept {
    size_t p = 1;
    while (p < v) p <<= 1;
    return p;
  }

  size_t capacity_{0};
  size_t mask_{0};
  std::vector<T> slots_;

  // Place the atomics in their own cache lines, as far apart as possible.
  alignas(64) std::atomic<uint64_t> writePos_{0};
  alignas(64) std::atomic<uint64_t> readPos_{0};
};

} // namespace rtplot
