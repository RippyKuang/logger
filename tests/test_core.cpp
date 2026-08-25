#include "rtplot/downsample.hpp"
#include "rtplot/logger.hpp"
#include "rtplot/ring_buffer.hpp"
#include "rtplot/shm_ipc.hpp"
#include "rtplot/storage.hpp"
#include "rtplot/udp.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace rtplot;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)) { std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; ++failures; } } while(0)

static void testRing() {
  RingBuffer<Sample> rb(1024);
  Sample tmp{};
  CHECK(!rb.tryRead(tmp));
  for (int i = 0; i < 2000; ++i) rb.tryWrite({i, double(i)});
  CHECK(rb.size() == 1024); // drop-newest default
  for (int i = 0; i < 100; ++i) CHECK(rb.tryRead(tmp));
  CHECK(rb.size() == 924);

  RingBuffer<Sample> rb2(16);
  std::atomic<bool> start{false};
  std::atomic<uint64_t> sum{0};
  std::thread producer([&] {
    while (!start.load()) {}
    for (int i = 0; i < 200000; ++i) {
      while (!rb2.tryWrite({i, double(i)})) {}
    }
  });
  std::thread consumer([&] {
    uint64_t s = 0;
    Sample x;
    while (!start.load()) {}
    for (int i = 0; i < 200000; ++i) {
      while (!rb2.tryRead(x)) {}
      s += uint64_t(x.v);
    }
    sum.store(s);
  });
  start.store(true);
  producer.join(); consumer.join();
  CHECK(sum.load() == 200000ull * 199999ull / 2ull);

  // Wait-free hot path should be tens of ns; the assertion is intentionally
  // generous (CI/VM noise) while still catching accidental blocking designs.
  RingBuffer<Sample> rb3(1 << 20);
  Sample s{nowNs(), 1.0};
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000000; ++i) rb3.tryWrite(s);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t0).count();
  std::cout << "[ring] 1M pushes in " << us << " us (" << us / 1000.0 << " ns/push)\n";
  CHECK(us < 200000); // < 200 ns/push
}

static void testStorage() {
  const std::string path = "/tmp/rtplot_test.db";
  ::remove(path.c_str());
  ::remove((path + ".wal").c_str());
  {
    StorageWriter w;
    CHECK(w.open(path));
    std::vector<Sample> a(1000), b(500);
    for (int i = 0; i < 1000; ++i) a[i] = {1000000000000 + i * 1000000, std::sin(i * 0.01)};
    for (int i = 0; i < 500; ++i) b[i] = {1000000000000 + i * 2000000, double(i)};
    CHECK(w.writeSamples("sin", a.data(), a.size()));
    CHECK(w.writeSamples("ramp", b.data(), b.size()));
    CHECK(w.writeEvent(Event{1000000500000, "State", "IDLE->RUN"}));
    CHECK(w.flush());
    // Second WAL transaction in the same writer session exercises the
    // checkpoint stream-position handling.
    std::vector<Sample> late(100);
    for (int i = 0; i < 100; ++i) late[i] = {1000000000000 + i * 5000000, double(i) * 2};
    CHECK(w.writeSamples("late", late.data(), late.size()));
    std::vector<ArraySample> arr(50);
    for (int i = 0; i < 50; ++i) { arr[i].t = 1000000000000 + i * 3000000; arr[i].size = 3; arr[i].values = {double(i), double(i*2), double(i*3)}; }
    CHECK(w.writeArraySamples("pose/position", arr.data(), arr.size()));
    CHECK(w.flush());
    w.close();
  }
  {
    StorageReader r;
    CHECK(r.open(path));
    const auto ch = r.channels();
    CHECK(ch.size() == 4);
    CHECK(r.readSamples("late").size() == 100);
    bool sawArray = false;
    for (const auto& ci : ch) if (ci.name == "pose/position") {
      CHECK(ci.isArray && ci.arrayLength == 3);
      sawArray = true;
    }
    CHECK(sawArray);
    const auto arrBack = r.readArraySamples("pose/position");
    CHECK(arrBack.size() == 50 && arrBack.back().size == 3);
    const auto sinAll = r.readSamples("sin");
    CHECK(sinAll.size() == 1000);
    const auto part = r.readSamples("sin", 1000100000000, 1000200000000);
    CHECK(!part.empty());
    for (const auto& s : part) CHECK(s.t >= 1000100000000 && s.t <= 1000200000000);
    const auto down = r.readSamples("sin", 0, INT64_MAX, 200);
    CHECK(down.size() <= 200);
    const auto stats = r.stats("ramp");
    CHECK(stats.count == 500);
    CHECK(std::abs(stats.min - 0.0) < 1e-12 && std::abs(stats.max - 499.0) < 1e-12);
    const auto evs = r.events();
    CHECK(evs.size() == 1 && evs[0].name == "State" && evs[0].payload == "IDLE->RUN");
    r.close();
  }
  CHECK(exportCsv(path, "/tmp/rtplot_test.csv"));
  std::cout << "[storage] write/read/range/index/stats/export OK\n";
}

static void testShm() {
  ::shm_unlink("/rtplot_ctrl_test");
  ::unlink("/tmp/rtplot_shm_rtplot_ctrl_test");
  for (int i = 0; i < 4; ++i) {
    for (const char* kind : {"data", "array"}) {
      const std::string posixName = std::string("/rtplot_") + kind + "_" + std::to_string(i);
      ::shm_unlink(posixName.c_str());
      ::unlink(("/tmp/rtplot_shm_rtplot_" + std::string(kind) + "_" + std::to_string(i)).c_str());
    }
  }
  {
    ShmPublisher pub;
    CHECK(pub.start("rtplot_ctrl_test", 256));
    std::vector<Sample> s(300);
    for (int i = 0; i < 300; ++i) s[i] = {1000 + i, double(i)};
    CHECK(pub.publish("ch0", s.data(), s.size()));
    std::vector<ArraySample> arr(10);
    for (int i = 0; i < 10; ++i) { arr[i].t = 1000 + i; arr[i].size = 3; arr[i].values = {double(i), double(i+1), double(i+2)}; }
    CHECK(pub.publishArray("pose/position", arr.data(), arr.size()));

    ShmReader rd;
    CHECK(rd.start("rtplot_ctrl_test"));
    const auto chans = rd.discover();
    CHECK(chans.size() == 2 && chans[0].name == "ch0");
    bool sawArr = false;
    for (const auto& c : chans) if (c.name == "pose/position") {
      CHECK(c.isArray && c.arrayLength == 3);
      sawArr = true;
    }
    CHECK(sawArr);
    std::vector<Sample> got;
    // Publisher drops oldest when no/lagging consumer: capacity is 256, so a
    // burst of 300 leaves the latest 256 values visible.
    CHECK(rd.read("ch0", got, 300) == 256);
    CHECK(got.front().v == 44.0);
    CHECK(got.back().v == 299.0);
    std::vector<ArraySample> gotArr;
    CHECK(rd.readArray("pose/position", gotArr, 10) == 10);
    CHECK(gotArr.back().size == 3 && gotArr.back().values[2] == 11.0);
    rd.stop();
    pub.stop();
  }
  std::cout << "[shm] control table + per-channel SPSC segment OK\n";
}

static void testUdp() {
  UdpReceiver rx;
  CHECK(rx.start(19870));
  {
    UdpPublisher tx;
    CHECK(tx.start("127.0.0.1", 19870));
    std::vector<Sample> s{{42, 1.5}, {43, 2.5}};
    CHECK(tx.publish("udp_ch", s.data(), s.size()));
    std::string ch;
    std::vector<Sample> got;
    bool ok = false;
    for (int i = 0; i < 200 && !ok; ++i) {
      ok = rx.receive(ch, got);
      if (!ok) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(ok && ch == "udp_ch" && got.size() == 2 && got[1].v == 2.5);
    tx.stop();
  }
  rx.stop();
  std::cout << "[udp] loopback packet OK\n";
}

static void testLogger() {
  const std::string path = "/tmp/rtplot_logger_test.db";
  ::remove(path.c_str()); ::remove((path + ".wal").c_str());
  {
    LoggerConfig cfg;
    cfg.persist = true;
    cfg.dbPath = path;
    cfg.flushIntervalMs = 1;
    cfg.ringCapacity = 4096;
    auto& log = Logger::instance();
    CHECK(log.start(cfg));
    for (int i = 0; i < 2000; ++i) log.log("j1", double(i));
    double qv[4] = {1, 0, 0, 0};
    for (int i = 0; i < 100; ++i) CHECK(log.logArray("pose/quaternion", qv, 4, 2000000000000 + i * 1000000));
    log.event("State", "RUNNING");
    log.flushAndStop();
    CHECK(!log.running());
  }
  StorageReader r;
  CHECK(r.open(path));
  CHECK(r.readSamples("j1").size() == 2000);
  CHECK(r.readArraySamples("pose/quaternion").size() == 100);
  CHECK(r.events().size() == 1);
  r.close();
  std::cout << "[logger] macro-style front-end -> background writer -> .db OK\n";
}

int main() {
  testRing();
  testStorage();
  testShm();
  testUdp();
  testLogger();
  if (failures) { std::cerr << failures << " check(s) failed\n"; return 1; }
  std::cout << "ALL CORE TESTS PASSED\n";
  return 0;
}
