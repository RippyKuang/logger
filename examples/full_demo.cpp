// rtplot full-featured C++ example.
//
// Demonstrates:
//   - SPSC RingBuffer (wait-free hot path)
//   - Logger scalar / array channels / events, .db persistence
//   - StorageReader random access + statistics
//   - CSV export
//   - MinMax / LTTB downsampling
//   - SHM publisher / reader (scalar + array)
//   - UDP publisher / receiver loopback
#include <rtplot/rtplot.hpp>

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace rtplot;

static bool shmRoundTrip() {
  ShmPublisher pub;
  ShmReader sub;
  if (!pub.start("rtplot_demo_ctrl", 256)) return false;
  if (!sub.start("rtplot_demo_ctrl")) return false;

  Sample scalar{nowNs(), 1.5};
  ArraySample pos;
  pos.t = nowNs();
  pos.size = 3;
  pos.values = {1.0, 2.0, 3.0};
  if (!pub.publish("demo/scalar", &scalar, 1)) return false;
  if (!pub.publishArray("demo/position", &pos, 1)) return false;

  auto chans = sub.discover();
  std::vector<Sample> scalarOut;
  std::vector<ArraySample> arrayOut;
  if (sub.read("demo/scalar", scalarOut, 1) != 1) return false;
  if (sub.readArray("demo/position", arrayOut, 1) != 1) return false;
  sub.stop();
  pub.stop();
  return !chans.empty() && scalarOut[0].v == 1.5 && arrayOut[0].size == 3;
}

static bool udpRoundTrip() {
  UdpReceiver rx;
  UdpPublisher tx;
  if (!rx.start(19876) || !tx.start("127.0.0.1", 19876)) return false;
  Sample s{nowNs(), 42.0};
  tx.publish("demo/udp", &s, 1);
  std::string channel;
  std::vector<Sample> out;
  for (int i = 0; i < 100; ++i) {
    if (rx.receive(channel, out)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  tx.stop();
  rx.stop();
  return !out.empty() && channel == "demo/udp" && out[0].v == 42.0;
}

int main() {
  // 1. SPSC ring: one producer, one consumer, no locks in the hot path.
  RingBuffer<Sample> ring(1024);
  std::thread producer([&] {
    for (int i = 0; i < 100000; ++i) {
      while (!ring.tryWrite(Sample{int64_t(i), double(i)})) {}
    }
  });
  uint64_t sum = 0;
  Sample item;
  for (int i = 0; i < 100000; ++i) {
    while (!ring.tryRead(item)) {}
    sum += uint64_t(item.v);
  }
  producer.join();
  std::printf("[ring] sum=%llu\n", static_cast<unsigned long long>(sum));

  // 2. Logger: scalar, array channels, events and .db persistence.
  LoggerConfig cfg;
  cfg.persist = true;
  cfg.shmPublish = true;
  cfg.dbPath = "full_demo.db";
  cfg.flushIntervalMs = 1;
  auto& log = Logger::instance();
  if (!log.start(cfg)) return 2;

  const Timestamp t0 = nowNs();
  for (int i = 0; i < 500; ++i) {
    const Timestamp t = t0 + i * 1000000LL;
    log.log("demo/sin", std::sin(i * 0.05), t);
    double pos[3] = {0, std::cos(i * 0.1), std::sin(i * 0.1)};
    log.logArray("demo/position", pos, 3, t);
    if (i % 100 == 0) log.event("StateChange", i < 250 ? "START" : "RUNNING", t);
  }
  log.flushAndStop();

  // 3. Read back, statistics and CSV export.
  StorageReader reader;
  if (!reader.open("full_demo.db")) return 3;
  const auto sinSamples = reader.readSamples("demo/sin");
  const auto positions = reader.readArraySamples("demo/position");
  const auto stats = reader.stats("demo/sin");
  std::printf("[db] sin=%zu pos=%zu events=%zu mean=%.3f\n",
              sinSamples.size(), positions.size(), reader.events().size(), stats.mean);
  if (!exportCsv("full_demo.db", "full_demo.csv")) return 4;

  // 4. Downsampling (100k -> 2000 points) with both MinMax and LTTB.
  std::vector<Sample> large;
  large.reserve(100000);
  for (int i = 0; i < 100000; ++i) large.push_back({t0 + i * 1000LL, std::sin(i * 0.001)});
  const auto mm = downsample(large, 2000, DownsampleAlgorithm::MinMax);
  const auto lttb = downsample(large, 2000, DownsampleAlgorithm::LTTB);
  std::printf("[downsample] minmax=%zu lttb=%zu\n", mm.size(), lttb.size());

  // 5. SHM and UDP round-trips.
  std::printf("[shm] %s\n", shmRoundTrip() ? "OK" : "FAILED");
  std::printf("[udp] %s\n", udpRoundTrip() ? "OK" : "FAILED");
  return 0;
}
