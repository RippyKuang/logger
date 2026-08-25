// SHM publisher example: run this process first, then run shm_viewer.
#include <rtplot/rtplot.hpp>

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <thread>

using namespace rtplot;
static std::atomic<bool> g_quit{false};

static void onSignal(int) { g_quit.store(true); }

int main() {
  LoggerConfig cfg;
  cfg.persist = false;           // no .db
  cfg.shmPublish = true;         // publish /rtplot_ctrl
  cfg.flushIntervalMs = 2;

  auto& log = Logger::instance();
  if (!log.start(cfg)) { std::fprintf(stderr, "logger start failed\n"); return 1; }
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  const Timestamp t0 = nowNs();
  for (uint64_t i = 0; !g_quit.load(); ++i) {
    const Timestamp t = t0 + static_cast<Timestamp>(i * 2000000ULL);
    const double x = static_cast<double>(i) * 0.002;
    log.log("demo/sin", std::sin(2.0 * M_PI * 0.5 * x), t);

    double pos[3] = {std::cos(x * 0.5), std::sin(x * 0.5), x * 0.01};
    log.logArray("pose/position", pos, 3, t);

    if (i % 250 == 0) log.event("StateChange", i % 500 ? "RUNNING" : "IDLE", t);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  log.flushAndStop();
  return 0;
}
