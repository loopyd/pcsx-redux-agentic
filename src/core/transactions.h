// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/history.h"
#include <atomic>

namespace PCSX::History::CD {

enum class Operation : uint32_t {
    CpuRead = 1, CpuWrite, HandlerRead, HandlerWrite, Command,
    Read, Play, Lid, LidSeek, Dma, DmaComplete, Decode, Fifo, Continuation, IsoScan,
};

bool idle();
bool active();
struct Limits {
    uint32_t structureBytes = 48, wireVersion = 3, contextBytes = 384, queueBytes = 32;
    uint32_t schedulerCount = 11, cdSchedulerMask = 0x41c, eventCeiling = 65536, payloadCeiling = 8388608;
    uint32_t minimumBoundaryEvents = 0, minimumBoundaryBytes = 0, workerRecordBytes = 80, workerEventCeiling = 65536;
};
static_assert(sizeof(Limits) == 48);
struct WorkerRecord {
    uint64_t sequence = 0, epoch = 0, batch = 0, sampleOrdinal = 0, cpuPublicationCycle = 0, relatedFeed = 0;
    uint32_t kind = 0, flags = 0;
    std::array<uint32_t, 6> data{};
};
static_assert(sizeof(WorkerRecord) == 80);
struct WorkerStatus {
    uint64_t epoch = 0, beginBatch = 0, endBatch = 0;
    uint32_t version = 3, state = 0, events = 0, limit = 0, failures = 0, dropped = 0;
};
static_assert(sizeof(WorkerStatus) == 48);
bool limits(uint32_t flags, Limits* out);
bool prepare(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags);
bool freezeWorker();
bool workerStatus(WorkerStatus* out);
bool workerRecord(uint32_t index, WorkerRecord* out);
inline uint32_t spuAddress(const void* base, const void* pointer) {
    const auto first = reinterpret_cast<uintptr_t>(base), value = reinterpret_cast<uintptr_t>(pointer);
    return first && value >= first && value-first < 524288 ? uint32_t(value-first) : UINT32_MAX;
}
void workerSample(uint32_t sample);
void workerObserve(uint32_t kind, std::array<uint32_t, 6> data, bool compress = false);
void publishSpuInterrupt(std::atomic<bool>& flag, uint32_t kind, uint32_t channel, uint32_t address, uint32_t control, uint32_t status);
bool takeSpuInterrupt(std::atomic<bool>& flag);
class WorkerBatch {
  public:
    WorkerBatch();
    ~WorkerBatch();
    void finish();
  private:
    bool m_observed = false;
};
uint64_t nextIdentity();
std::array<uint64_t, 8> identities();
std::array<uint64_t, 4> scheduleOwners();
void submission();
void stream(uint32_t type);
void schedulerPass(uint32_t mask, uint64_t lowest);
void schedulerTest(uint32_t slot, uint32_t mask, uint32_t live, uint64_t target, bool dispatch);
void schedulerChange(uint32_t slot, uint32_t mask, uint64_t oldTarget, uint64_t newTarget, uint32_t scale, bool cancel);
void dataBoundary(uint32_t phase, const void* bytes, uint32_t length);
void dataMutation(uint32_t buffer, uint32_t offset, uint32_t length, const void* bytes, uint64_t media = 0);
void consume(uint32_t offset, uint32_t length, const void* bytes, uint32_t address, bool dma);
bool dmaActive();
bool isoScan();
void dmaBegin(uint32_t address, uint32_t offset, uint32_t length);
void dmaEnd(uint32_t returned);
void dmaReceipt(uint32_t address, uint32_t length, const void* source, uint32_t copied, uint32_t destination, uint32_t destinationOffset);
void configure(bool enabled, uint64_t epoch, bool joined, uint64_t audioEpoch);
bool validate();
void boundary(uint32_t phase);
void invalidate();
uint64_t epoch();
uint64_t audioEpoch();
bool joined();
uint64_t operation();
uint64_t access();

enum class Predicate : uint32_t {
    ReadIRQ = 1, Buffer, Subchannel, SubQ, SBI, Missing, Status, SpuControl, SpuAddress,
    Availability, SubchannelState, NativeBranch, XaDecode, Playback, LidState,
};
void predicate(Predicate kind, std::array<uint32_t, 8> values = {},
               const void* payload = nullptr, uint32_t length = 0);

uint64_t backing();
void mediaQuery(uint32_t kind, uint32_t argument, uint32_t result, const void* payload = nullptr, uint32_t length = 0);
void mediaOutcome(uint32_t reason, int64_t count, uint32_t track = 0, uint32_t file = 0,
                  uint32_t sector = 0, uint32_t flags = 0, uint32_t base = 0);
class Media {
  public:
    Media(uint32_t kind, uint32_t msf, uint32_t lba, uint64_t prior);
    ~Media();
    void result(bool success, uint32_t disposition);
    uint64_t id() const { return m_id; }
    Media(const Media&) = delete;
    Media& operator=(const Media&) = delete;
  private:
    uint64_t m_id = 0, m_epoch = 0;
    uint32_t m_kind = 0;
    bool m_finished = false;
};

class Feed {
  public:
    Feed(bool cdda, uint64_t media, uint64_t mutation, uint64_t stream,
         uint32_t rate, uint32_t frames, uint32_t stereo);
    ~Feed();
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;
  private:
    uint64_t m_id = 0, m_epoch = 0;
};
bool feedActive();
void feedOutcome(uint32_t reason, uint32_t rate, uint32_t sourceFrames, uint32_t stereo,
                 uint32_t outputFrames, bool cdda, uint64_t audioState, uint64_t audioEpoch, uint64_t event);

class Scope {
  public:
    explicit Scope(Operation kind, uint32_t address = 0, uint32_t width = 0, uint32_t value = 0);
    ~Scope();
    void result(uint32_t value) { m_value = value; m_returned = true; }
    bool observed() const { return m_id != 0; }
    bool root() const { return m_id && !m_parent; }
    uint64_t id() const { return m_id; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    void emit(uint32_t phase);
    Operation m_kind;
    uint32_t m_address, m_width, m_value;
    uint64_t m_epoch = 0, m_id = 0, m_parent = 0;
    bool m_entered = false, m_returned = false;
};

}
