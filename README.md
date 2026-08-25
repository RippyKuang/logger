# rtplot — 轻量实时数据可视化与日志库

C++17 + Qt 的嵌入式实时曲线/日志库。前端记录为 SPSC 无锁环形队列，
后台线程统一完成 `.db` 落盘、SHM 共享内存发布和 UDP 推流；Qt viewer
以独立进程显示/回放，并支持直接记录模式。

---

## 1. 项目结构

```text
include/rtplot/
  rtplot.hpp          # 统一头文件（推荐 include）
  types.hpp           # Sample / ArraySample / Event / ChannelInfo / Stats
  ring_buffer.hpp     # SPSC 无锁队列
  downsample.hpp      # MinMax / LTTB / Decimate
  logger.hpp          # Logger 单例与 RTPLOT_LOG / RTPLOT_EVENT 宏
  storage.hpp         # .db 读写、索引、CSV 导出
  shm_ipc.hpp         # POSIX 共享内存发布/订阅
  udp.hpp             # UDP 推流/接收
src/                  # Core 实现（不依赖 Qt）
gui/                  # Qt viewer：曲线、ROI、事件、3D/Pose、侧边栏
bindings/python.cpp   # pybind11：import rtplot
apps/viewer.cpp       # 统一 viewer 入口
examples/full_demo.cpp / full_demo.py
tests/test_core.cpp
```

## 2. 构建、安装与运行

```bash
cmake -S . -B build \
  -DRTPLOT_BUILD_GUI=ON \
  -DRTPLOT_BUILD_PYTHON=ON \
  -DRTPLOT_BUILD_DEMOS=ON \
  -DRTPLOT_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

安装到系统或自定义前缀：

```bash
# 安装到 /usr/local
cmake --install build

# 或安装到自定义目录
cmake --install build --prefix $HOME/.local
```

卸载：

```bash
# 根据 install_manifest.txt 删除已安装文件
cmake --build build --target uninstall

# 或直接运行脚本
./scripts/uninstall.sh build/install_manifest.txt
```

安装内容：

```text
lib/       librtplot_core.a / librtplot_gui.a
bin/       viewer / shm_recorder / shm_viewer
include/   rtplot/*.hpp, rtplot/gui/*.hpp
lib/cmake/ rtplotConfig.cmake + rtplotTargets.cmake
```

安装后，其他项目可通过 CMake 使用：

```cmake
find_package(rtplot CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE rtplot::core)   # 核心库
target_link_libraries(gui_target PRIVATE rtplot::gui)     # Qt GUI 库
```

`examples/` 目录包含独立的 `CMakeLists.txt`，不会使用源码目录，而是查找已安装的 `rtplot`：

```bash
cd examples
cmake -S . -B build_examples \
  -DCMAKE_PREFIX_PATH=$HOME/.local
cmake --build build_examples -j
```

如果安装到 `/usr/local`，可以直接：

```bash
cd examples
cmake -S . -B build_examples
cmake --build build_examples -j
```

运行统一 viewer：

```bash
# 离线回放
./build/viewer run.db

# 加载示例数据
./build/viewer --demo

# SHM 实时显示（默认）
./build/viewer --source shm --shm-name rtplot_ctrl

# UDP 实时显示
./build/viewer --source udp --udp-port 9870

# 直接记录并显示（结束后自动打开生成的 .db）
./build/viewer --source record --record-db run.db --record-duration 10
```

C++ 和 Python 完整示例：

```bash
./build/rtplot_example
PYTHONPATH=build python3 examples/full_demo.py
```

SHM 实时显示最简示例（终端 1 发布，终端 2 显示）：

```bash
./build/shm_recorder
./build/shm_viewer
```

## 3. C++ 接口与示例

统一头文件：

```cpp
#include <rtplot/rtplot.hpp>
using namespace rtplot;
```

### 3.1 Logger：标量、数组、事件

```cpp
LoggerConfig cfg;
cfg.persist = true;       // 写 .db
cfg.dbPath = "run.db";
cfg.shmPublish = true;    // 同时发布到 SHM，供 viewer 实时显示
cfg.shmName = "rtplot_ctrl"; // 默认即可，也可自定义与 viewer --shm-name 对应
cfg.udpPublish = true;    // 同时 UDP 推流
cfg.flushIntervalMs = 2;

Logger::instance().start(cfg);

Logger::instance().log("joint/pos", 1.234);
RTPLOT_LOG("joint/vel", 0.42);

double position[3] = {1, 2, 3};
Logger::instance().logArray("pose/position", position, 3);

double quat[4] = {1, 0, 0, 0};
Logger::instance().logArray("pose/quaternion", quat, 4);

Logger::instance().event("StateChange", "IDLE -> RUNNING");
Logger::instance().flushAndStop();
```

数组长度自动解释：

- `3`：Euler R/P/Y；
- `4`：Quaternion `(w, x, y, z)`；
- 长度至少为 3 的数组也可作为位置/轨迹。

### 3.2 SPSC 无锁队列

```cpp
RingBuffer<Sample> rb(1 << 16);
rb.tryWrite({nowNs(), 1.0});
Sample s;
while (!rb.tryRead(s)) {}
```

### 3.3 存储与回放

```cpp
StorageWriter w;
w.open("run.db");
w.writeSamples("joint/pos", samples.data(), samples.size());
w.writeArraySamples("pose/position", arrays.data(), arrays.size());
w.writeEvent(Event{nowNs(), "State", "RUNNING"});
w.flush();

StorageReader r;
r.open("run.db");
for (const auto& ci : r.channels()) {
  if (ci.isArray) {
    auto arr = r.readArraySamples(ci.name);
  } else {
    auto samples = r.readSamples(ci.name, t0, t1, /*maxPoints=*/100000);
    Stats st = r.stats(ci.name);
  }
}
auto events = r.events();
exportCsv("run.db", "run.csv");
```

### 3.4 降采样

```cpp
auto minmax = downsample(samples, 2000, DownsampleAlgorithm::MinMax);
auto lttb   = downsample(samples, 2000, DownsampleAlgorithm::LTTB);
```

### 3.5 SHM 发布/订阅

```cpp
ShmPublisher pub;
pub.start("rtplot_ctrl");
pub.publish("joint/pos", samples.data(), samples.size());
pub.publishArray("pose/position", arrays.data(), arrays.size());

ShmReader sub;
sub.start("rtplot_ctrl");
for (const auto& ch : sub.discover()) {
  if (ch.isArray) {
    std::vector<ArraySample> out;
    sub.readArray(ch.name, out);
  } else {
    std::vector<Sample> out;
    sub.read(ch.name, out);
  }
}
```

### 3.6 UDP 推流/接收

```cpp
UdpPublisher tx;
tx.start("127.0.0.1", 9870);
tx.publish("joint/pos", samples.data(), samples.size());

UdpReceiver rx;
rx.start(9870);
std::string channel;
std::vector<Sample> out;
while (!rx.receive(channel, out)) { /* spin / sleep */ }
```

## 4. Python 接口

接口与 C++ 一一对应：

```python
import rtplot

cfg = rtplot.LoggerConfig()
cfg.persist = True
cfg.db_path = "run.db"
cfg.flush_interval_ms = 1

log = rtplot.Logger.instance()
log.start(cfg)
log.log("joint/pos", 1.23)
log.log_array("pose/position", [1.0, 2.0, 3.0])
log.event("StateChange", "RUNNING")
log.flush_and_stop()

r = rtplot.StorageReader()
r.open("run.db")
print(r.read_samples("joint/pos"))
print(r.read_array_samples("pose/position"))
print(r.stats("joint/pos").mean)
rtplot.export_csv("run.db", "run.csv")
```

## 5. Viewer 功能

- 统一 `viewer`：SHM / UDP / 直接记录 / `.db` 回放。
- 可拖拽右侧 Dock：
  - Time series：channel 勾选列表、单通道 ROI 统计下拉框。
  - 3D/Pose：Frame 列表，Position array / Orientation array 选择。
- 曲线初始不显示，勾选后显示；同一 `/` 前缀自动分到同一子图。
- 空子图自动删除；没有曲线时保留一个空图。
- 左键框选缩放、右键 ROI、右键双击删除 ROI。
- 悬停显示 `t / v`，右键单击设置参考点，左键双击删除参考点。
- 事件时间轴：StateChange 纵向标记与 Tooltip，不受曲线显隐影响。
- 3D：蓝色地面网格、右手系、只显示已走过的轨迹、多 Frame 姿态。

## 6. 代码编写指南

1. 包含 `rtplot/rtplot.hpp`，全部类型位于 `namespace rtplot`。
2. 实时线程只调用 `log / logArray / event`；它们只做无锁 ring push。
3. 后台写线程由 `LoggerConfig` 配置，统一负责 `.db` / SHM / UDP。
4. 标量通道用 `Sample {t, v}`；数组通道用 `ArraySample`（最大长度 16）。
5. Viewer 通过 `--source` 选择数据源；外部记录进程设置 `shmPublish=true`。
6. 3D/Pose 使用数组通道：长度 3 自动识别为 Euler，长度 4 自动识别为四元数。
7. `.db` 是单写者模型；关闭前调用 `flush()` 或 `flushAndStop()`。
8. GUI 曲线分组无需配置：channel 名 `/` 前的部分即前缀，同一前缀自动共用子图。

## 7. 测试

```bash
ctest --test-dir build --output-on-failure
```

`tests/test_core.cpp` 覆盖：SPSC 队列、Logger 标量/数组/事件落盘、
StorageReader 区间读取与统计、CSV 导出、SHM 标量/数组、UDP 回环。
