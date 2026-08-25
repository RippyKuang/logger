# rtplot Python example. Mirrors the C++ API.
import math
import os
import rtplot

# 1. Logger: scalar + array channels + events, persisted to .db.
for path in ("full_demo_py.db", "full_demo_py.db.wal"):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass

cfg = rtplot.LoggerConfig()
cfg.persist = True
cfg.db_path = "full_demo_py.db"
cfg.flush_interval_ms = 1

log = rtplot.Logger.instance()
assert log.start(cfg)
for i in range(500):
    log.log("demo/sin", math.sin(i * 0.05))
    log.log_array("demo/position", [float(i), math.cos(i * 0.1), math.sin(i * 0.1)])
    if i % 100 == 0:
        log.event("StateChange", "START" if i < 250 else "RUNNING")
log.flush_and_stop()

# 2. Read back + statistics + CSV export.
reader = rtplot.StorageReader()
assert reader.open("full_demo_py.db")
samples = reader.read_samples("demo/sin")
positions = reader.read_array_samples("demo/position")
stats = reader.stats("demo/sin")
print(f"[db] sin={len(samples)} pos={len(positions)} events={len(reader.events())} mean={stats.mean:.3f}")
assert rtplot.export_csv("full_demo_py.db", "full_demo_py.csv")

# 3. Downsampling.
big = [rtplot.Sample(i, math.sin(i * 0.001)) for i in range(100000)]
mm = rtplot.downsample(big, 2000, rtplot.DownsampleAlgorithm.MinMax)
lttb = rtplot.downsample(big, 2000, rtplot.DownsampleAlgorithm.LTTB)
print(f"[downsample] minmax={len(mm)} lttb={len(lttb)}")
