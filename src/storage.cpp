#include "rtplot/storage.hpp"
#include "rtplot/downsample.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_map>

#include <fcntl.h>
#include <unistd.h>

namespace rtplot {
namespace {

constexpr uint32_t kFileMagic = 0x30424452u; // 'RDB0' bytes below are checked manually
constexpr uint32_t kFrameMagic = 0x46524D31u; // 'FRM1'
constexpr uint32_t kMetaMagic = 0x4D455441u;  // 'META'
constexpr uint32_t kTxnMagic = 0x54584E31u;   // 'TXN1'
constexpr uint32_t kEventMagic = 0x45565431u; // 'EVT1'
constexpr uint32_t kVersion = 2;
constexpr uint32_t kHeaderSize = 512;
constexpr uint32_t kMaxMetadata = 64u << 20;

#pragma pack(push, 1)
struct FileHeader {
  char magic[8];
  uint32_t version;
  uint32_t headerSize;
  uint64_t createdNs;
  uint64_t dataEnd;
  uint64_t metadataOffset;
  uint64_t metadataSize;
  uint32_t flags;
  uint32_t headerCrc;
  char reserved[512 - 56];
};
static_assert(sizeof(FileHeader) == kHeaderSize, "FileHeader layout");

struct FrameHeader {
  uint32_t magic;
  uint32_t channelId;
  uint32_t sampleCount;
  uint32_t flags;
  uint64_t startTs;
  uint64_t endTs;
  uint32_t payloadBytes;
  uint32_t frameCrc;
};
static_assert(sizeof(FrameHeader) == 40, "FrameHeader layout");

struct EventRecord {
  uint32_t magic;
  uint32_t nameLen;
  uint32_t payloadLen;
  uint64_t ts;
};

struct TxnHeader {
  uint32_t magic;
  uint32_t payloadBytes;
  uint32_t crc;
};
#pragma pack(pop)

constexpr uint8_t kRecSample = 1;
constexpr uint8_t kRecEvent = 2;
constexpr uint8_t kRecArray = 3;

uint32_t crc32(const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int b = 0; b < 8; ++b) {
      const uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

void appendU32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void appendU64(std::vector<uint8_t>& v, uint64_t x) {
  for (int i = 0; i < 8; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
void appendI64(std::vector<uint8_t>& v, int64_t x) { appendU64(v, static_cast<uint64_t>(x)); }

uint32_t readU32(const uint8_t*& p) {
  uint32_t x = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
               (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  p += 4; return x;
}
uint64_t readU64(const uint8_t*& p) {
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) x |= uint64_t(p[i]) << (8 * i);
  p += 8; return x;
}
int64_t readI64(const uint8_t*& p) { return static_cast<int64_t>(readU64(p)); }

struct FrameIndex {
  uint32_t channelId = 0;
  uint64_t offset = 0;       // offset of the record type byte
  uint32_t sampleCount = 0;
  uint32_t payloadBytes = 0;
  int64_t startTs = 0;
  int64_t endTs = 0;
  uint32_t crc = 0;
};

struct EventIndex {
  uint64_t offset = 0;       // offset of the record type byte
  uint32_t size = 0;
  int64_t ts = 0;
};

struct ChannelMeta {
  uint32_t id = 0;
  std::string name;
  uint64_t sampleCount = 0;
  int64_t firstTs = 0;
  int64_t lastTs = 0;
  bool isArray = false;
  uint32_t arrayLength = 0;
  std::vector<FrameIndex> frames;
};

std::vector<uint8_t> makeSampleRecord(uint32_t channelId, const Sample* s, size_t count) {
  std::vector<uint8_t> payload;
  payload.reserve(1 + sizeof(FrameHeader) + count * sizeof(Sample));
  payload.push_back(kRecSample);
  FrameHeader fh{};
  fh.magic = kFrameMagic;
  fh.channelId = channelId;
  fh.sampleCount = static_cast<uint32_t>(count);
  fh.startTs = count ? s[0].t : 0;
  fh.endTs = count ? s[count - 1].t : 0;
  fh.payloadBytes = static_cast<uint32_t>(count * sizeof(Sample));
  fh.frameCrc = count ? crc32(s, count * sizeof(Sample)) : 0;
  const uint8_t* fp = reinterpret_cast<const uint8_t*>(&fh);
  payload.insert(payload.end(), fp, fp + sizeof(fh));
  if (count) {
    const uint8_t* sp = reinterpret_cast<const uint8_t*>(s);
    payload.insert(payload.end(), sp, sp + count * sizeof(Sample));
  }
  return payload;
}

std::vector<uint8_t> makeArrayRecord(uint32_t channelId, const ArraySample* s, size_t count) {
  std::vector<uint8_t> body;
  size_t totalPayload = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint32_t n = std::min<uint32_t>(s[i].size, kMaxArrayLength);
    totalPayload += 8 + 4 + size_t(n) * 8;
  }
  body.reserve(1 + sizeof(FrameHeader) + totalPayload);
  body.push_back(kRecArray);
  FrameHeader fh{};
  fh.magic = kFrameMagic;
  fh.channelId = channelId;
  fh.sampleCount = static_cast<uint32_t>(count);
  fh.startTs = count ? s[0].t : 0;
  fh.endTs = count ? s[count - 1].t : 0;
  fh.payloadBytes = static_cast<uint32_t>(totalPayload);
  const uint8_t* fp = reinterpret_cast<const uint8_t*>(&fh);
  body.insert(body.end(), fp, fp + sizeof(fh));
  for (size_t i = 0; i < count; ++i) {
    const uint32_t n = std::min<uint32_t>(s[i].size, kMaxArrayLength);
    appendI64(body, s[i].t);
    appendU32(body, n);
    for (uint32_t j = 0; j < n; ++j) {
      uint64_t bits = 0;
      std::memcpy(&bits, &s[i].values[j], sizeof(bits));
      appendU64(body, bits);
    }
  }
  return body;
}

std::vector<uint8_t> makeEventRecord(const Event& ev) {
  std::vector<uint8_t> payload;
  payload.reserve(1 + sizeof(EventRecord) + ev.name.size() + ev.payload.size());
  payload.push_back(kRecEvent);
  EventRecord er{};
  er.magic = kEventMagic;
  er.nameLen = static_cast<uint32_t>(ev.name.size());
  er.payloadLen = static_cast<uint32_t>(ev.payload.size());
  er.ts = ev.t;
  const uint8_t* ep = reinterpret_cast<const uint8_t*>(&er);
  payload.insert(payload.end(), ep, ep + sizeof(er));
  payload.insert(payload.end(), ev.name.begin(), ev.name.end());
  payload.insert(payload.end(), ev.payload.begin(), ev.payload.end());
  return payload;
}

} // namespace

// ============================================================================
// StorageWriter
// ============================================================================
class StorageWriter::Impl {
public:
  std::mutex mtx;
  std::string path, walPath;
  FILE* mainFile = nullptr;
  FILE* walFile = nullptr;
  uint64_t dataEnd = kHeaderSize;
  size_t walBytes = 0;
  StorageConfig cfg;
  uint64_t createdNs = 0;

  std::vector<ChannelMeta> channels;
  std::unordered_map<std::string, uint32_t> idByChannel;
  uint32_t nextId = 1;
  std::vector<EventIndex> eventIndex;

  uint32_t channelId(const std::string& name) {
    auto it = idByChannel.find(name);
    if (it != idByChannel.end()) return it->second;
    ChannelMeta cm;
    cm.id = nextId++;
    cm.name = name;
    channels.push_back(cm);
    idByChannel.emplace(name, cm.id);
    return cm.id;
  }

  bool appendWal(const std::vector<uint8_t>& record) {
    TxnHeader th{};
    th.magic = kTxnMagic;
    th.payloadBytes = static_cast<uint32_t>(record.size());
    th.crc = crc32(record.data(), record.size());
    if (std::fwrite(&th, sizeof(th), 1, walFile) != 1) return false;
    if (std::fwrite(record.data(), 1, record.size(), walFile) != record.size()) return false;
    walBytes += sizeof(th) + record.size();
    return true;
  }

  bool applyRecord(const uint8_t* p, size_t size) {
    if (size < 2) return false;
    const uint8_t type = p[0];
    ++p; --size;
    if (type == kRecSample) {
      if (size < sizeof(FrameHeader)) return false;
      FrameHeader fh{};
      std::memcpy(&fh, p, sizeof(fh));
      if (fh.magic != kFrameMagic) return false;
      const size_t expected = sizeof(FrameHeader) + size_t(fh.sampleCount) * sizeof(Sample);
      if (size != expected) return false;
      const uint8_t* sampleBytes = p + sizeof(FrameHeader);
      if (fh.frameCrc != crc32(sampleBytes, size_t(fh.sampleCount) * sizeof(Sample))) return false;

      ChannelMeta* cm = nullptr;
      for (auto& c : channels) if (c.id == fh.channelId) { cm = &c; break; }
      if (!cm) return false;

      const Sample* samples = reinterpret_cast<const Sample*>(sampleBytes);
      FrameIndex fi;
      fi.channelId = fh.channelId;
      fi.offset = dataEnd;
      fi.sampleCount = fh.sampleCount;
      fi.payloadBytes = fh.payloadBytes;
      fi.startTs = fh.startTs;
      fi.endTs = fh.endTs;
      fi.crc = fh.frameCrc;
      cm->frames.push_back(fi);
      cm->sampleCount += fh.sampleCount;
      if (fh.sampleCount) {
        cm->firstTs = cm->firstTs == 0 ? samples[0].t : std::min<int64_t>(cm->firstTs, samples[0].t);
        cm->lastTs = std::max<int64_t>(cm->lastTs, samples[fh.sampleCount - 1].t);
      }
    } else if (type == kRecArray) {
      if (size < sizeof(FrameHeader)) return false;
      FrameHeader fh{};
      std::memcpy(&fh, p, sizeof(fh));
      if (fh.magic != kFrameMagic || fh.payloadBytes > size - sizeof(FrameHeader)) return false;
      const uint8_t* ap = p + sizeof(FrameHeader);
      const uint8_t* aend = p + sizeof(FrameHeader) + fh.payloadBytes;
      int64_t first = 0, last = 0;
      uint32_t len0 = 0;
      for (uint32_t i = 0; i < fh.sampleCount; ++i) {
        if (ap + 12 > aend) return false;
        const int64_t t = readI64(ap);
        const uint32_t n = readU32(ap);
        if (n > kMaxArrayLength || ap + size_t(n) * 8 > aend) return false;
        ap += size_t(n) * 8;
        if (i == 0) { first = t; len0 = n; }
        last = t;
      }
      if (ap != aend) return false;

      ChannelMeta* cm = nullptr;
      for (auto& c : channels) if (c.id == fh.channelId) { cm = &c; break; }
      if (!cm) return false;
      cm->isArray = true;
      cm->arrayLength = std::max(cm->arrayLength, len0);
      FrameIndex fi;
      fi.channelId = fh.channelId;
      fi.offset = dataEnd;
      fi.sampleCount = fh.sampleCount;
      fi.payloadBytes = fh.payloadBytes;
      fi.startTs = first;
      fi.endTs = last;
      fi.crc = 0;
      cm->frames.push_back(fi);
      cm->sampleCount += fh.sampleCount;
      if (fh.sampleCount) {
        cm->firstTs = cm->firstTs == 0 ? first : std::min<int64_t>(cm->firstTs, first);
        cm->lastTs = std::max<int64_t>(cm->lastTs, last);
      }
    } else if (type == kRecEvent) {
      if (size < sizeof(EventRecord)) return false;
      EventRecord er{};
      std::memcpy(&er, p, sizeof(er));
      if (er.magic != kEventMagic) return false;
      const size_t expected = sizeof(EventRecord) + size_t(er.nameLen) + size_t(er.payloadLen);
      if (size != expected) return false;
      EventIndex ei;
      ei.offset = dataEnd;
      ei.size = static_cast<uint32_t>(size + 1);
      ei.ts = er.ts;
      eventIndex.push_back(ei);
    } else {
      return false;
    }
    dataEnd += size + 1;
    return true;
  }

  bool recoverFromMain() {
    FileHeader fh{};
    if (std::fseek(mainFile, 0, SEEK_SET) != 0) return false;
    if (std::fread(&fh, sizeof(fh), 1, mainFile) != 1) return false;
    if (std::memcmp(fh.magic, "RTPLDB01", 8) != 0 || fh.version > kVersion) return false;
    dataEnd = fh.dataEnd;
    createdNs = fh.createdNs;
    if (fh.metadataSize == 0) return true;

    if (fh.metadataSize > kMaxMetadata) return false;
    std::vector<uint8_t> meta(fh.metadataSize);
    if (std::fseek(mainFile, static_cast<long>(fh.metadataOffset), SEEK_SET) != 0) return false;
    if (std::fread(meta.data(), 1, meta.size(), mainFile) != meta.size()) return false;
    if (meta.size() < 20) return false;
    const uint8_t* crcPtr = meta.data() + meta.size() - 4;
    if (readU32(crcPtr) != crc32(meta.data(), meta.size() - 4)) return false;

    const uint8_t* p = meta.data();
    const uint8_t* end = meta.data() + meta.size() - 4;
    if (readU32(p) != kMetaMagic) return false;
    const uint32_t channelCount = readU32(p);
    const uint32_t frameCount = readU32(p);
    const uint32_t eventCount = readU32(p);
    channels.clear(); idByChannel.clear(); eventIndex.clear(); nextId = 1;
    channels.reserve(channelCount);
    for (uint32_t i = 0; i < channelCount; ++i) {
      if (p + 4 > end) return false;
      ChannelMeta cm;
      cm.id = readU32(p);
      if (p + 4 > end) return false;
      const uint32_t nameLen = readU32(p);
      if (p + 24 > end) return false;
      cm.sampleCount = readU64(p);
      cm.firstTs = readI64(p);
      cm.lastTs = readI64(p);
      if (fh.version >= 2) {
        if (p + 8 > end) return false;
        cm.isArray = readU32(p) != 0;
        cm.arrayLength = readU32(p);
      }
      if (nameLen > 1024 || p + nameLen > end) return false;
      cm.name.assign(reinterpret_cast<const char*>(p), nameLen);
      p += nameLen;
      idByChannel[cm.name] = cm.id;
      nextId = std::max(nextId, cm.id + 1);
      channels.push_back(std::move(cm));
    }
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (p + 40 > end) return false;
      FrameIndex fi;
      fi.channelId = readU32(p);
      fi.offset = readU64(p);
      fi.sampleCount = readU32(p);
      fi.payloadBytes = readU32(p);
      fi.startTs = readI64(p);
      fi.endTs = readI64(p);
      fi.crc = readU32(p);
      for (auto& c : channels) if (c.id == fi.channelId) c.frames.push_back(fi);
    }
    for (uint32_t i = 0; i < eventCount; ++i) {
      if (p + 16 > end) return false;
      EventIndex ei;
      ei.offset = readU64(p);
      ei.size = readU32(p);
      ei.ts = readI64(p);
      eventIndex.push_back(ei);
    }
    return true;
  }

  bool recoverWalLocked() {
    if (!walFile) return false;
    if (std::fseek(walFile, 0, SEEK_END) != 0) return false;
    const long size = std::ftell(walFile);
    if (size == 0) return true;
    std::rewind(walFile);
    uint64_t validEnd = 0, pos = 0;
    std::vector<uint8_t> buf;
    while (true) {
      TxnHeader th{};
      if (std::fread(&th, sizeof(th), 1, walFile) != 1) break;
      pos += sizeof(th);
      if (th.magic != kTxnMagic || th.payloadBytes == 0 || th.payloadBytes > (64u << 20)) break;
      buf.resize(th.payloadBytes);
      if (std::fread(buf.data(), 1, buf.size(), walFile) != buf.size()) break;
      pos += buf.size();
      if (th.crc != crc32(buf.data(), buf.size())) break;
      if (std::fwrite(buf.data(), 1, buf.size(), mainFile) != buf.size()) break;
      if (!applyRecord(buf.data(), buf.size())) {
        if (::ftruncate(fileno(mainFile), static_cast<off_t>(dataEnd)) != 0) return false;
        break;
      }
      validEnd = pos;
    }
    if (std::fseek(walFile, 0, SEEK_SET) != 0) return false;
    if (::ftruncate(fileno(walFile), static_cast<off_t>(validEnd)) != 0) return false;
    std::fflush(walFile);
    std::fseek(walFile, 0, SEEK_END);
    walBytes = 0;
    return true;
  }

  bool checkpoint() {
    if (!walFile || walBytes == 0) return true;
    std::fflush(walFile);
    const int wfd = fileno(walFile);
    if (wfd >= 0) ::fdatasync(wfd);

    // The main-file stream currently points after the previous metadata block;
    // new WAL records are appended there and indexed from this offset.
    if (std::fseek(mainFile, 0, SEEK_END) != 0) return false;
    dataEnd = static_cast<uint64_t>(std::ftell(mainFile));

    std::rewind(walFile);
    uint64_t validEnd = 0;
    uint64_t pos = 0;
    std::vector<uint8_t> buf;
    while (true) {
      TxnHeader th{};
      const size_t got = std::fread(&th, sizeof(th), 1, walFile);
      if (got != 1) break;
      pos += sizeof(th);
      if (th.magic != kTxnMagic || th.payloadBytes == 0 || th.payloadBytes > (64u << 20)) break;
      buf.resize(th.payloadBytes);
      if (std::fread(buf.data(), 1, buf.size(), walFile) != buf.size()) break;
      pos += buf.size();
      if (th.crc != crc32(buf.data(), buf.size())) break;
      if (std::fwrite(buf.data(), 1, buf.size(), mainFile) != buf.size()) break;
      if (!applyRecord(buf.data(), buf.size())) {
        // Roll back partial main-file write: this should not happen for our
        // own generated WAL; truncate main to the pre-transaction offset.
        std::fclose(mainFile);
        mainFile = std::fopen(path.c_str(), "r+b");
        if (mainFile && ::ftruncate(fileno(mainFile), static_cast<off_t>(dataEnd)) != 0) return false;
        std::fseek(mainFile, 0, SEEK_END);
        break;
      }
      validEnd = pos;
    }

    // Truncate checkpointed WAL records.
    if (validEnd < pos) {
      std::fclose(walFile);
      walFile = std::fopen(walPath.c_str(), "r+b");
      if (!walFile) return false;
      if (::ftruncate(fileno(walFile), static_cast<off_t>(validEnd)) != 0) return false;
      std::fseek(walFile, static_cast<long>(validEnd), SEEK_SET);
    } else {
      if (::ftruncate(fileno(walFile), 0) != 0) return false;
      std::rewind(walFile);
    }
    std::fflush(walFile);
    walBytes = 0;
    return writeMetadata();
  }

  bool writeMetadata() {
    std::sort(eventIndex.begin(), eventIndex.end(),
              [](const EventIndex& a, const EventIndex& b) { return a.ts < b.ts; });
    for (auto& c : channels) {
      std::sort(c.frames.begin(), c.frames.end(),
                [](const FrameIndex& a, const FrameIndex& b) { return a.startTs < b.startTs; });
    }

    std::vector<uint8_t> meta;
    meta.reserve(1u << 16);
    appendU32(meta, kMetaMagic);
    appendU32(meta, static_cast<uint32_t>(channels.size()));
    appendU32(meta, 0); // total frames, patched below
    appendU32(meta, static_cast<uint32_t>(eventIndex.size()));

    uint32_t frameCount = 0;
    for (const auto& c : channels) {
      appendU32(meta, c.id);
      appendU32(meta, static_cast<uint32_t>(c.name.size()));
      appendU64(meta, c.sampleCount);
      appendI64(meta, c.firstTs);
      appendI64(meta, c.lastTs);
      appendU32(meta, c.isArray ? 1u : 0u);
      appendU32(meta, c.arrayLength);
      meta.insert(meta.end(), c.name.begin(), c.name.end());
      frameCount += static_cast<uint32_t>(c.frames.size());
    }
    // Patch frame count (magic[0..4], channelCount[4..8], frameCount[8..12]).
    meta[8] = frameCount & 0xFF;
    meta[9] = (frameCount >> 8) & 0xFF;
    meta[10] = (frameCount >> 16) & 0xFF;
    meta[11] = (frameCount >> 24) & 0xFF;

    for (const auto& c : channels) {
      for (const auto& f : c.frames) {
        appendU32(meta, f.channelId);
        appendU64(meta, f.offset);
        appendU32(meta, f.sampleCount);
        appendU32(meta, f.payloadBytes);
        appendI64(meta, f.startTs);
        appendI64(meta, f.endTs);
        appendU32(meta, f.crc);
      }
    }
    for (const auto& e : eventIndex) {
      appendU64(meta, e.offset);
      appendU32(meta, e.size);
      appendI64(meta, e.ts);
    }
    appendU32(meta, crc32(meta.data(), meta.size()));

    if (meta.size() > kMaxMetadata) return false;
    if (std::fseek(mainFile, static_cast<long>(dataEnd), SEEK_SET) != 0) return false;
    if (std::fwrite(meta.data(), 1, meta.size(), mainFile) != meta.size()) return false;

    FileHeader fh{};
    std::memcpy(fh.magic, "RTPLDB01", 8);
    fh.version = kVersion;
    fh.headerSize = kHeaderSize;
    fh.createdNs = createdNs;
    fh.dataEnd = dataEnd;
    fh.metadataOffset = dataEnd;
    fh.metadataSize = meta.size();
    fh.flags = 0;
    fh.headerCrc = crc32(&fh, offsetof(FileHeader, headerCrc));
    std::fflush(mainFile);
    const int fd = fileno(mainFile);
    if (fd >= 0) ::fdatasync(fd);
    if (std::fseek(mainFile, 0, SEEK_SET) != 0) return false;
    if (std::fwrite(&fh, sizeof(fh), 1, mainFile) != 1) return false;
    std::fflush(mainFile);
    if (fd >= 0) ::fdatasync(fd);
    std::fseek(mainFile, 0, SEEK_END);
    return true;
  }
};

StorageWriter::StorageWriter() : impl_(std::make_unique<Impl>()) {}
StorageWriter::~StorageWriter() { close(); }

bool StorageWriter::open(const std::string& path, const StorageConfig& cfg) {
  close();
  std::lock_guard<std::mutex> lk(impl_->mtx);
  impl_->path = path;
  impl_->walPath = path + ".wal";
  impl_->cfg = cfg;
  impl_->createdNs = nowNs();
  impl_->dataEnd = kHeaderSize;
  impl_->walBytes = 0;

  const bool existed = (::access(path.c_str(), F_OK) == 0);
  impl_->mainFile = std::fopen(path.c_str(), existed ? "r+b" : "w+b");
  if (!impl_->mainFile) return false;
  if (existed) {
    if (!impl_->recoverFromMain()) return false;
    if (std::fseek(impl_->mainFile, 0, SEEK_END) != 0) return false;
    impl_->dataEnd = static_cast<uint64_t>(std::ftell(impl_->mainFile));
  } else {
    FileHeader fh{};
    std::memset(&fh, 0, sizeof(fh));
    std::memcpy(fh.magic, "RTPLDB01", 8);
    fh.version = kVersion;
    fh.headerSize = kHeaderSize;
    fh.createdNs = impl_->createdNs;
    fh.dataEnd = impl_->dataEnd;
    fh.metadataOffset = impl_->dataEnd;
    fh.metadataSize = 0;
    if (std::fwrite(&fh, sizeof(fh), 1, impl_->mainFile) != 1) return false;
    std::fseek(impl_->mainFile, static_cast<long>(kHeaderSize), SEEK_SET);
  }

  impl_->walFile = std::fopen(impl_->walPath.c_str(), "r+b");
  if (!impl_->walFile) impl_->walFile = std::fopen(impl_->walPath.c_str(), "w+b");
  if (!impl_->walFile) return false;
  if (!impl_->recoverWalLocked()) return false;
  std::fseek(impl_->mainFile, 0, SEEK_END);
  return true;
}

void StorageWriter::close() {
  flush();
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (impl_->walFile) { std::fclose(impl_->walFile); impl_->walFile = nullptr; }
  if (impl_->mainFile) { std::fclose(impl_->mainFile); impl_->mainFile = nullptr; }
  impl_->channels.clear();
  impl_->idByChannel.clear();
  impl_->eventIndex.clear();
  impl_->nextId = 1;
  impl_->dataEnd = kHeaderSize;
  impl_->walBytes = 0;
}

bool StorageWriter::isOpen() const noexcept { return impl_ && impl_->mainFile != nullptr; }

bool StorageWriter::writeSamples(const std::string& channel, const Sample* samples,
                                 size_t count, Timestamp) {
  if (!samples || count == 0) return true;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (!impl_->mainFile || !impl_->walFile) return false;
  const uint32_t id = impl_->channelId(channel);
  auto rec = makeSampleRecord(id, samples, count);
  return impl_->appendWal(rec);
}

bool StorageWriter::writeArraySamples(const std::string& channel, const ArraySample* samples,
                                         size_t count, Timestamp) {
  if (!samples || count == 0) return true;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (!impl_->mainFile || !impl_->walFile) return false;
  const uint32_t id = impl_->channelId(channel);
  for (auto& c : impl_->channels) {
    if (c.id == id) {
      c.isArray = true;
      if (count) c.arrayLength = std::max(c.arrayLength, samples[0].size);
    }
  }
  auto rec = makeArrayRecord(id, samples, count);
  return impl_->appendWal(rec);
}

bool StorageWriter::writeEvent(const Event& ev) {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (!impl_->mainFile || !impl_->walFile) return false;
  return impl_->appendWal(makeEventRecord(ev));
}

bool StorageWriter::flush() {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lk(impl_->mtx);
  if (!impl_->mainFile || !impl_->walFile) return true;
  if (impl_->walBytes == 0) return impl_->writeMetadata();
  return impl_->checkpoint();
}

void StorageWriter::setFlushInterval(uint32_t ms) noexcept {
  if (impl_) impl_->cfg.flushIntervalMs = ms;
}
const std::string& StorageWriter::path() const noexcept { return impl_->path; }

// ============================================================================
// StorageReader
// ============================================================================
class StorageReader::Impl {
public:
  std::string path;
  std::vector<ChannelMeta> channels;
  std::unordered_map<std::string, size_t> indexByChannel;
  std::vector<EventIndex> events;

  bool parseMetadata(FILE* f) {
    FileHeader fh{};
    if (std::fread(&fh, sizeof(fh), 1, f) != 1) return false;
    if (std::memcmp(fh.magic, "RTPLDB01", 8) != 0 || fh.version > kVersion) return false;
    if (fh.metadataSize == 0 || fh.metadataSize > kMaxMetadata) return false;
    std::vector<uint8_t> meta(fh.metadataSize);
    if (std::fseek(f, static_cast<long>(fh.metadataOffset), SEEK_SET) != 0) return false;
    if (std::fread(meta.data(), 1, meta.size(), f) != meta.size()) return false;
    if (meta.size() < 20) return false;
    const uint8_t* crcPtr = meta.data() + meta.size() - 4;
    const uint32_t storedCrc = readU32(crcPtr);
    if (storedCrc != crc32(meta.data(), meta.size() - 4)) return false;

    const uint8_t* p = meta.data();
    const uint8_t* end = meta.data() + meta.size() - 4;
    if (readU32(p) != kMetaMagic) return false;
    const uint32_t channelCount = readU32(p);
    const uint32_t frameCount = readU32(p);
    const uint32_t eventCount = readU32(p);
    channels.reserve(channelCount);
    for (uint32_t i = 0; i < channelCount; ++i) {
      if (p + 4 > end) return false;
      ChannelMeta cm;
      cm.id = readU32(p);
      if (p + 4 > end) return false;
      const uint32_t nameLen = readU32(p);
      if (p + 24 > end) return false;
      cm.sampleCount = readU64(p);
      cm.firstTs = readI64(p);
      cm.lastTs = readI64(p);
      if (fh.version >= 2) {
        if (p + 8 > end) return false;
        cm.isArray = readU32(p) != 0;
        cm.arrayLength = readU32(p);
      }
      if (nameLen > 1024 || p + nameLen > end) return false;
      cm.name.assign(reinterpret_cast<const char*>(p), nameLen);
      p += nameLen;
      channels.push_back(std::move(cm));
    }
    for (uint32_t i = 0; i < frameCount; ++i) {
      if (p + 32 > end) return false;
      FrameIndex fi;
      fi.channelId = readU32(p);
      fi.offset = readU64(p);
      fi.sampleCount = readU32(p);
      fi.payloadBytes = readU32(p);
      fi.startTs = readI64(p);
      fi.endTs = readI64(p);
      fi.crc = readU32(p);
      for (auto& c : channels) if (c.id == fi.channelId) c.frames.push_back(fi);
    }
    for (uint32_t i = 0; i < eventCount; ++i) {
      if (p + 16 > end) return false;
      EventIndex ei;
      ei.offset = readU64(p);
      ei.size = readU32(p);
      ei.ts = readI64(p);
      events.push_back(ei);
    }
    for (size_t i = 0; i < channels.size(); ++i) indexByChannel[channels[i].name] = i;
    return true;
  }

  const ChannelMeta* find(const std::string& name) const {
    auto it = indexByChannel.find(name);
    return it == indexByChannel.end() ? nullptr : &channels[it->second];
  }

  const FrameIndex* firstFrameAtOrAfter(const ChannelMeta& cm, int64_t t) const {
    const auto& f = cm.frames;
    if (f.empty()) return nullptr;
    if (t <= f.front().startTs) return &f.front();
    size_t lo = 0, hi = f.size();
    while (lo + 1 < hi) {
      const size_t mid = (lo + hi) / 2;
      if (f[mid].startTs < t) lo = mid; else hi = mid;
    }
    if (hi < f.size()) return &f[hi];
    return f.back().endTs >= t ? &f.back() : nullptr;
  }

  bool readFramePayload(FILE* f, const FrameIndex& fi, std::vector<uint8_t>& buf) const {
    buf.resize(size_t(fi.sampleCount) * sizeof(Sample));
    if (fi.sampleCount == 0) return true;
    if (std::fseek(f, static_cast<long>(fi.offset + 1 + sizeof(FrameHeader)), SEEK_SET) != 0) return false;
    return std::fread(buf.data(), 1, buf.size(), f) == buf.size();
  }
};

StorageReader::StorageReader() : impl_(std::make_unique<Impl>()) {}
StorageReader::~StorageReader() { close(); }

bool StorageReader::open(const std::string& path) {
  close();
  impl_->path = path;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  const bool ok = impl_->parseMetadata(f);
  std::fclose(f);
  return ok;
}
void StorageReader::close() {
  impl_->channels.clear();
  impl_->indexByChannel.clear();
  impl_->events.clear();
  impl_->path.clear();
}
bool StorageReader::isOpen() const noexcept { return !impl_->channels.empty() || !impl_->path.empty(); }

std::vector<ChannelInfo> StorageReader::channels() const {
  std::vector<ChannelInfo> out;
  for (const auto& c : impl_->channels) {
    ChannelInfo ci;
    ci.id = c.id; ci.name = c.name; ci.sampleCount = c.sampleCount;
    ci.firstTs = c.firstTs; ci.lastTs = c.lastTs;
    ci.isArray = c.isArray; ci.arrayLength = c.arrayLength;
    out.push_back(std::move(ci));
  }
  return out;
}

bool StorageReader::hasChannel(const std::string& name) const {
  return impl_->find(name) != nullptr;
}

std::vector<Sample> StorageReader::readSamples(const std::string& channel, Timestamp t0,
                                               Timestamp t1, size_t maxSamples) const {
  std::vector<Sample> out;
  const ChannelMeta* cm = impl_->find(channel);
  if (!cm) return out;
  FILE* f = std::fopen(impl_->path.c_str(), "rb");
  if (!f) return out;
  const FrameIndex* first = impl_->firstFrameAtOrAfter(*cm, t0);
  if (!first) { std::fclose(f); return out; }
  const size_t begin = static_cast<size_t>(first - cm->frames.data());
  std::vector<uint8_t> buf;
  size_t gathered = 0;
  for (size_t idx = begin; idx < cm->frames.size(); ++idx) {
    const FrameIndex& fi = cm->frames[idx];
    if (fi.startTs > t1) break;
    if (fi.endTs < t0) continue;
    if (!impl_->readFramePayload(f, fi, buf)) break;
    const Sample* s = reinterpret_cast<const Sample*>(buf.data());
    for (size_t i = 0; i < fi.sampleCount; ++i) {
      if (s[i].t >= t0 && s[i].t <= t1) {
        out.push_back(s[i]);
        ++gathered;
        if (maxSamples && gathered >= maxSamples * 4) {
          std::fclose(f);
          return downsampleMinMax(out, maxSamples);
        }
      }
    }
  }
  std::fclose(f);
  if (maxSamples && out.size() > maxSamples) return downsampleMinMax(out, maxSamples);
  return out;
}

std::vector<ArraySample> StorageReader::readArraySamples(const std::string& channel, Timestamp t0,
                                                         Timestamp t1, size_t maxSamples) const {
  std::vector<ArraySample> out;
  const ChannelMeta* cm = impl_->find(channel);
  if (!cm || !cm->isArray) return out;
  FILE* f = std::fopen(impl_->path.c_str(), "rb");
  if (!f) return out;
  const FrameIndex* first = impl_->firstFrameAtOrAfter(*cm, t0);
  if (!first) { std::fclose(f); return out; }
  const size_t begin = static_cast<size_t>(first - cm->frames.data());
  std::vector<uint8_t> buf;
  for (size_t idx = begin; idx < cm->frames.size(); ++idx) {
    const FrameIndex& fi = cm->frames[idx];
    if (fi.startTs > t1) break;
    if (fi.endTs < t0) continue;
    buf.resize(fi.payloadBytes);
    if (std::fseek(f, static_cast<long>(fi.offset + 1 + sizeof(FrameHeader)), SEEK_SET) != 0) break;
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) break;
    const uint8_t* p = buf.data();
    const uint8_t* end = buf.data() + buf.size();
    for (uint32_t i = 0; i < fi.sampleCount; ++i) {
      if (p + 12 > end) break;
      ArraySample s;
      s.t = readI64(p);
      s.size = readU32(p);
      if (s.size > kMaxArrayLength || p + size_t(s.size) * 8 > end) break;
      for (uint32_t j = 0; j < s.size; ++j) {
        uint64_t bits = readU64(p);
        std::memcpy(&s.values[j], &bits, sizeof(double));
      }
      if (s.t >= t0 && s.t <= t1) out.push_back(s);
    }
  }
  std::fclose(f);
  if (maxSamples && out.size() > maxSamples) {
    const double step = static_cast<double>(out.size()) / static_cast<double>(maxSamples);
    std::vector<ArraySample> ds;
    ds.reserve(maxSamples);
    for (size_t i = 0; i < maxSamples; ++i) ds.push_back(out[static_cast<size_t>(i * step)]);
    return ds;
  }
  return out;
}

std::vector<Event> StorageReader::events(Timestamp t0, Timestamp t1) const {
  std::vector<Event> out;
  FILE* f = std::fopen(impl_->path.c_str(), "rb");
  if (!f) return out;
  for (const auto& ei : impl_->events) {
    if (ei.ts < t0 || ei.ts > t1) continue;
    if (ei.size < 1 + sizeof(EventRecord)) continue;
    std::vector<uint8_t> buf(ei.size);
    if (std::fseek(f, static_cast<long>(ei.offset), SEEK_SET) != 0) continue;
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) continue;
    if (buf[0] != kRecEvent) continue;
    EventRecord er{};
    std::memcpy(&er, buf.data() + 1, sizeof(er));
    if (er.magic != kEventMagic || sizeof(EventRecord) + er.nameLen + er.payloadLen + 1 != buf.size()) continue;
    Event ev;
    ev.t = er.ts;
    ev.name.assign(reinterpret_cast<const char*>(buf.data() + 1 + sizeof(EventRecord)), er.nameLen);
    ev.payload.assign(reinterpret_cast<const char*>(buf.data() + 1 + sizeof(EventRecord) + er.nameLen), er.payloadLen);
    out.push_back(std::move(ev));
  }
  std::fclose(f);
  return out;
}

Stats StorageReader::stats(const std::string& channel, Timestamp t0, Timestamp t1) const {
  Stats st;
  const ChannelMeta* cm = impl_->find(channel);
  if (!cm) return st;
  FILE* f = std::fopen(impl_->path.c_str(), "rb");
  if (!f) return st;
  const FrameIndex* first = impl_->firstFrameAtOrAfter(*cm, t0);
  if (!first) { std::fclose(f); return st; }
  const size_t begin = static_cast<size_t>(first - cm->frames.data());
  std::vector<uint8_t> buf;
  double sum = 0.0, sum2 = 0.0;
  for (size_t idx = begin; idx < cm->frames.size(); ++idx) {
    const FrameIndex& fi = cm->frames[idx];
    if (fi.startTs > t1) break;
    if (fi.endTs < t0) continue;
    if (!impl_->readFramePayload(f, fi, buf)) break;
    const Sample* s = reinterpret_cast<const Sample*>(buf.data());
    for (size_t i = 0; i < fi.sampleCount; ++i) {
      if (s[i].t < t0 || s[i].t > t1) continue;
      st.count++;
      st.min = std::min(st.min, s[i].v);
      st.max = std::max(st.max, s[i].v);
      sum += s[i].v; sum2 += s[i].v * s[i].v;
    }
  }
  std::fclose(f);
  if (st.count) {
    st.mean = sum / static_cast<double>(st.count);
    st.variance = st.count > 1 ? (sum2 - sum * st.mean) / static_cast<double>(st.count - 1) : 0.0;
    if (st.variance < 0) st.variance = 0;
    st.stddev = std::sqrt(st.variance);
    st.rms = std::sqrt(sum2 / static_cast<double>(st.count));
  }
  return st;
}

ChannelInfo StorageReader::info(const std::string& channel) const {
  const ChannelMeta* cm = impl_->find(channel);
  ChannelInfo ci;
  if (!cm) return ci;
  ci.id = cm->id; ci.name = cm->name; ci.sampleCount = cm->sampleCount;
  ci.firstTs = cm->firstTs; ci.lastTs = cm->lastTs;
  return ci;
}

} // namespace rtplot
