# 架构与数据流

## 目标

实时记录线程的 `log()` 延迟控制在 10µs 以内，磁盘、SHM、UDP 和 GUI
全部移出实时路径。系统由四个模块组成：Core、Storage、GUI、Bindings。

## 热路径

`Logger::log(channel, value, t)`：

1. 惰性/显式 `start()` 保证后台管线就绪。
2. `ChannelRegistry::getOrCreate(channel)`：
   - FNV-1a 哈希 + 4096 槽开放寻址；
   - 命中路径为无锁原子指针 acquire 加载与字符串比较；
   - 未命中才使用管理互斥锁创建 `ChannelEntry`（首次记录某通道时发生，
     可在初始化阶段 `log` 一次预热）。
3. 调用 `RingBuffer<Sample>::write()`：
   - `readPos_.load(acquire)`；
   - 写入槽位；
   - `writePos_.store(release)`。
   - 无 CAS、无系统调用。

## 后台写线程

```
wait_for(flushIntervalMs, default 2ms)
drain events (mutex deque)       // 事件低频，不走无锁热路径
for each active channel:
    ring.copyAvailable(batch)    // SPSC 消费
    StorageWriter.writeSamples() // WAL append
    ShmPublisher.publish()       // POSIX shm SPSC push
    UdpPublisher.publish()       // sendto (non-blocking)
flush checkpoint when requested / on stop
```

- 所有 sink 共享同一个批量 `Sample` buffer，减少分配。
- `flushAndStop()` 使用条件变量请求 flush，后台线程最终 drain 后
  checkpoint 并退出。

## 进程间通信

控制段 `/rtplot_ctrl`：

- `ShmControlHeader`：magic、version、publisher pid。
- `ShmChannelSlot[64]`：`nameHash`、`state`、`pushed`、channel name、
  data segment name。发布顺序为 payload 先、`state` release 后；
  读取端 acquire 后即可安全解析。

数据段 `/rtplot_data_<slot>`：

- `ShmDataHeader`：magic、capacity、`writePos`、`readPos`。
- 随后为 `capacity` 个 `Sample` 槽位。
- 发布端满时 `DropOldest`（推进共享 readPos），保证直播显示看到最新数据。
- 读取端 `read()` 批量 copy 后 release 更新共享 `readPos`。

沙箱退化：若 `shm_open(O_CREAT)` 返回 `EACCES`，自动使用
`/tmp/rtplot_shm_*` 的 `open + mmap(MAP_SHARED)` 文件映射，接口不变。

## UDP 包格式

```
[0..4)  magic 0x52545055
[4..8)  sequence
[8..12) channel name length
[12..16) sample count
[16..24) first timestamp
[24..24+N) channel name bytes
随后 N * (int64 t, double v)，全部显式小端编码
```

## 渲染路径

1. `PlotStrip` 保存原始 `std::vector<Sample>`。
2. 每次 `paintEvent` 取当前 X 可视范围，用 `lower_bound/upper_bound`
   截取子区间。
3. 按 `2 * 像素宽度` 为目标点数执行 MinMax 或 LTTB。
4. `QPolygonF + QPainter::drawPolyline`，关闭 painter 级抗锯齿，
   线条 cosmetic pen。
5. 鼠标交互只更新共享 X 范围和各 strip Y 范围，触发 `update()`。

## 回放

`MainWindow` 在打开文件时把全部通道载入内存；`QTimer` 以 33ms 周期推进
`tCurrent += (tEnd-tStart) * 0.033 * speed`，再用二分截取前缀并交给
`PlotGridWidget`。进度条为 0..100000 的 int 映射，避免浮点 slider 问题。
