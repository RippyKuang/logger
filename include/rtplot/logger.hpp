#pragma once
#include "rtplot/types.hpp"
#include "rtplot/ring_buffer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rtplot {

struct LoggerConfig {
  /// In-memory ring capacity per channel (front-end buffer).
  size_t ringCapacity = 1u << 16;
  OverflowPolicy overflowPolicy = OverflowPolicy::DropNewest;

  /// Persistence (.db custom binary store, WAL + batched transactions).
  bool persist = false;
  std::string dbPath = "rtplot.db";

  /// Shared-memory IPC publication for an external Qt viewer process.
  bool shmPublish = false;
  size_t shmRingCapacity = 1u << 16;
  std::string shmName = "rtplot_ctrl";

  /// Remote UDP streaming (lightweight binary packets).
  bool udpPublish = false;
  std::string udpAddress = "127.0.0.1";
  uint16_t udpPort = 9870;

  /// Background writer thread.
  uint32_t flushIntervalMs = 2;
  bool startBackgroundThread = true;
  bool dropOldestWhenFull = false;
};

/// Process-wide singleton logger.
///
/// Front-end calls (`log`) only perform a wait-free SPSC ring push and return
/// immediately; a dedicated background thread owns persistence, SHM and UDP.
///
/// Minimal usage:
///   rtplot::Logger::instance().start({.persist=true, .dbPath="run.db"});
///   rtplot::Logger::instance().log("joint1/pos", 1.25);
class Logger {
public:
  static Logger& instance();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  /// Configure and start the background writer. Calling start() more than once
  /// with a different configuration stops the previous pipeline first.
  bool start(const LoggerConfig& cfg = {});
  void stop();
  [[nodiscard]] bool running() const noexcept;

  /// Non-blocking front-end call. Returns false when the sample was dropped
  /// because of the overflow policy.
  bool log(std::string_view channel, double value, Timestamp t = nowNs());

  /// Array-channel front-end call (position, quaternion, Euler angles, ...).
  /// Values are copied inline into a fixed-capacity ArraySample; no heap
  /// allocation occurs on the real-time path.
  bool logArray(std::string_view channel, const double* values, size_t size,
                Timestamp t = nowNs());

  /// Store a named event with an optional payload (e.g. a state transition).
  /// The GUI renders it as a vertical marker + tooltip on the time axis.
  bool event(std::string_view name, std::string_view payload,
             Timestamp t = nowNs());

  /// Blocking convenience: stops the pipeline after flushing pending samples.
  void flushAndStop();

  /// Peek a snapshot from the in-memory ring without consuming it. Useful for
  /// a same-process lightweight GUI and unit tests.
  size_t snapshot(std::string_view channel, std::vector<Sample>& out,
                  size_t maxSamples = 1u << 20) const;
  size_t snapshotArray(std::string_view channel, std::vector<ArraySample>& out,
                       size_t maxSamples = 1u << 20) const;
  size_t channelCount() const;

  const LoggerConfig& config() const noexcept;

  /// Drop counters since start.
  uint64_t acceptedSamples() const noexcept;
  uint64_t droppedSamples() const noexcept;

private:
  Logger();
  ~Logger();

  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// RAII duration logger: logs elapsed time (seconds) on scope exit.
class ScopedTimer {
public:
  ScopedTimer(std::string channel) : channel_(std::move(channel)) {
    start_ = std::chrono::steady_clock::now();
  }
  ~ScopedTimer() {
    const double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_).count();
    Logger::instance().log(channel_, sec);
  }
  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;
private:
  std::string channel_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace rtplot

/// Non-intrusive macros. `RTPLOT_LOGGER()` may also be used to configure the
/// singleton before logging starts.
#define RTPLOT_LOGGER() ::rtplot::Logger::instance()
#define RTPLOT_LOG(channel, value) \
  ::rtplot::Logger::instance().log((channel), (value))
#define RTPLOT_EVENT(name, payload) \
  ::rtplot::Logger::instance().event((name), (payload))
#define RTPLOT_LOG_ARRAY(channel, values, size) \
  ::rtplot::Logger::instance().logArray((channel), (values), (size))
#define RTPLOT_SCOPED_TIMER(channel) \
  ::rtplot::ScopedTimer rtplot_scoped_timer_##__LINE__((channel))
