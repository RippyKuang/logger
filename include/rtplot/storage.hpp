#pragma once
#include "rtplot/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rtplot {

struct StorageConfig {
  /// Flush interval hint used by Logger's background writer.
  uint32_t flushIntervalMs = 20;
  /// Force a checkpoint when the WAL exceeds this size (bytes).
  size_t maxWalBytes = 8u << 20;
};

/// Custom binary .db writer.
///
/// File layout:
///   [FileHeader][Frame][Frame]...[Metadata block]
/// A frame belongs to exactly one channel and stores a batch of timestamp/value
/// pairs. Batches are first appended to a side WAL file (.db.wal) as one
/// transaction; checkpointing copies the transaction to the main file, updates
/// channel/frame indexes and truncates the WAL.
class StorageWriter {
public:
  StorageWriter();
  ~StorageWriter();
  StorageWriter(const StorageWriter&) = delete;
  StorageWriter& operator=(const StorageWriter&) = delete;

  bool open(const std::string& path, const StorageConfig& cfg = {});
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  /// Append a batch as one transaction. Thread-safe; call from one writer
  /// thread for maximum throughput (a mutex is used for API safety).
  bool writeSamples(const std::string& channel, const Sample* samples,
                    size_t count, Timestamp now = nowNs());
  bool writeArraySamples(const std::string& channel, const ArraySample* samples,
                         size_t count, Timestamp now = nowNs());
  bool writeEvent(const Event& ev);

  /// Flush WAL and create/update the on-disk indexes.
  bool flush();
  void setFlushInterval(uint32_t ms) noexcept;

  [[nodiscard]] const std::string& path() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read-only random access over a .db file. Channel and frame indexes are
/// loaded from the metadata block with O(1) channel lookup and O(log N) frame
/// lookup by timestamp.
class StorageReader {
public:
  StorageReader();
  ~StorageReader();
  StorageReader(const StorageReader&) = delete;
  StorageReader& operator=(const StorageReader&) = delete;

  bool open(const std::string& path);
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  [[nodiscard]] std::vector<ChannelInfo> channels() const;
  [[nodiscard]] bool hasChannel(const std::string& name) const;

  /// Read samples in [t0,t1]. When maxSamples is exhausted early the returned
  /// vector is downsampled with MinMax so the visual envelope is preserved.
  [[nodiscard]] std::vector<Sample> readSamples(const std::string& channel,
                                                Timestamp t0 = 0,
                                                Timestamp t1 = INT64_MAX,
                                                size_t maxSamples = 0) const;

  [[nodiscard]] std::vector<Event> events(Timestamp t0 = 0,
                                          Timestamp t1 = INT64_MAX) const;
  [[nodiscard]] std::vector<ArraySample> readArraySamples(const std::string& channel,
                                                          Timestamp t0 = 0,
                                                          Timestamp t1 = INT64_MAX,
                                                          size_t maxSamples = 0) const;
  [[nodiscard]] Stats stats(const std::string& channel,
                            Timestamp t0 = 0,
                            Timestamp t1 = INT64_MAX) const;

  /// Cheap O(log channel-frames) bounds query.
  [[nodiscard]] ChannelInfo info(const std::string& channel) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Export helpers.
bool exportCsv(const std::string& dbPath, const std::string& csvPath,
               char delimiter = ',');

} // namespace rtplot
