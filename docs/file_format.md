# .db 二进制格式 v1

所有整数均为 little-endian，所有 reserved/padding 写 0。

## FileHeader（512 字节，packed）

| 偏移 | 字段 | 大小 |
|---|---|---|
| 0 | magic `"RTPLDB01"` | 8 |
| 8 | version = 2（兼容读取 v1） | u32 |
| 12 | headerSize = 512 | u32 |
| 16 | createdNs | u64 |
| 24 | dataEnd（数据区结束，不含 metadata） | u64 |
| 32 | metadataOffset | u64 |
| 40 | metadataSize | u64 |
| 48 | flags | u32 |
| 52 | headerCrc | u32 |
| 56 | reserved | 456 |

## 数据记录

数据区是类型化记录序列，从 headerSize 开始。每条记录：

```
u8  recordType   // 1 = SampleFrame, 2 = EventRecord, 3 = ArrayFrame
... payload
```

### SampleFrame

```
u32 magic 0x46524D31 "FRM1"
u32 channelId
u32 sampleCount
u32 flags
i64 startTs
i64 endTs
u32 payloadBytes = sampleCount * 16
u32 frameCrc          // CRC32(sample data)
Sample[sampleCount]   // {i64 t; f64 v;}
```

### ArrayFrame

```
u32 magic 0x46524D31 "FRM1"
u32 channelId
u32 sampleCount
u32 flags
i64 startTs
i64 endTs
u32 payloadBytes
u32 reserved
For each sample:
  i64 t
  u32 length N (<= 16)
  f64 values[N]
```

### EventRecord

```
u32 magic 0x45565431 "EVT1"
u32 nameLen
u32 payloadLen
i64 ts
u8  name[nameLen]
u8  payload[payloadLen]
```

## Metadata block（追加在 dataEnd 之后）

```
u32 magic 0x4D455441 "META"
u32 channelCount
u32 frameCount
u32 eventCount
channel directory:
  for each channel:
    u32 id
    u32 nameLen
    u64 sampleCount
    i64 firstTs
    i64 lastTs
    u32 isArray
    u32 arrayLength
    u8  name[nameLen]
frame index:
  for each frame:
    u32 channelId
    u64 offset       // 该记录 type 字节的绝对偏移
    u32 sampleCount
    u32 payloadBytes
    i64 startTs
    i64 endTs
    u32 crc
event index:
  for each event:
    u64 offset
    u32 size
    i64 ts
u32 blockCrc         // CRC32(block[0..end-4])
```

读取：加载 Header → 读 Metadata → 按通道建 hash map，帧索引按 startTs
排序；`readSamples(t0,t1)` 先二分找到首个 `startTs >= t0` 的帧，
再顺序读取重叠帧。

## WAL 事务

`<db>.wal` 由事务顺序追加：

```
u32 magic 0x54584E31 "TXN1"
u32 payloadBytes
u32 crc              // CRC32(payload)
payload              // 与主文件数据记录完全相同的 record bytes
```

checkpoint：`fdatasync(WAL)` → 逐条 CRC 校验并复制到主文件 → 更新内存索引
→ `ftruncate(WAL)` → 写 Metadata → `fdatasync(main)` → 更新 FileHeader。
打开已有文件时自动重放 WAL；如果尾部事务不完整则截断丢弃。
