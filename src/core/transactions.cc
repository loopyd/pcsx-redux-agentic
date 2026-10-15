// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/transactions.h"

#include <bit>
#include <cmath>
#include <condition_variable>
#include "core/provenance.h"

#include "core/psxemulator.h"
#include "core/cdrom.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/spu.h"

namespace PCSX::History::CD {
namespace {
struct Frame { Operation kind{}; uint64_t id = 0; };
std::array<Frame, 16> stack{};
uint32_t depth = 0;
uint64_t generation = 0, serial = 0, audioGeneration = 0;
uint64_t medium = 0;
uint64_t backingSelection = 0;
uint32_t mediaOutcomes = 0;
uint64_t feeding = 0;
bool feedFinished = false, feedCdda = false;
bool enabled = false, joinAudio = false;
SPU::Environment environment;
std::weak_ptr<CDRIso> disc;
uint32_t clock = 0;
std::array<uint32_t, 11> scales{};
bool isPort(Operation kind) { return kind <= Operation::HandlerWrite; }
bool isRead(Operation kind) { return kind == Operation::CpuRead || kind == Operation::HandlerRead; }
bool nested(Operation parent, Operation child) {
    return (parent == Operation::CpuRead && child == Operation::HandlerRead) ||
           (parent == Operation::CpuWrite && child == Operation::HandlerWrite) ||
           (parent == Operation::Dma && child == Operation::DmaComplete) ||
           (parent == Operation::Lid && child == Operation::LidSeek) ||
           (parent == Operation::Read && child == Operation::Play) ||
           ((parent == Operation::Fifo || parent == Operation::Continuation) && child == Operation::Command) ||
           ((parent == Operation::Command || parent == Operation::Read || parent == Operation::Play ||
             parent == Operation::Fifo) && child == Operation::Lid);
}
}

namespace {
struct WorkerStore {
    std::mutex mutex;
    std::condition_variable boundary;
    std::vector<WorkerRecord> records;
    WorkerStatus status;
    std::thread::id thread;
    uint64_t batch = 0, first = 0, last = 0, pendingEpoch = 0, freezeBatch = 0;
    uint32_t pendingCount = 0, pendingFlags = 0;
    bool enabled = false, inBatch = false, freeze = false;
};
WorkerStore worker;
std::atomic<uint64_t> cpuPublication{0}, feedPublication{0};
thread_local uint64_t workerBatch = 0;
thread_local uint32_t workerOrdinal = 0;
uint32_t preparedFlags = 0, preparedWorkerLimit = 0;
uint64_t currentStream = 0, latestMutation = 0, latestSubmission = 0, currentPass = 0;
std::array<uint64_t, 4> cdScheduleOwners{};
Origins fifoOrigins;
std::array<uint8_t, Origins::Size> fifoShadow{};
bool fifoShadowReady = false;
uint32_t dmaAddress = 0, dmaOffset = 0, dmaLength = 0, dmaReceived = 0;
bool observingDma = false;
int cdSlot(uint32_t slot) {
    constexpr std::array<uint32_t, 4> slots{2, 3, 4, 10};
    for (unsigned n = 0; n < slots.size(); ++n) if (slots[n] == slot) return n;
    return -1;
}
void little(uint8_t* out, uint64_t value, uint32_t length) {
    for (uint32_t n = 0; n < length; ++n) out[n] = value >> (n * 8);
}
void workerFailure(Failure failure) {
    worker.status.failures |= uint32_t(failure);
    if (worker.status.dropped != UINT32_MAX) ++worker.status.dropped;
}
void workerAppend(uint32_t kind, std::array<uint32_t, 6> data, bool compress = false) {
    if (!worker.enabled || (worker.freeze && workerBatch != worker.freezeBatch)) return;
    if (worker.thread != std::this_thread::get_id()) {
        workerFailure(Failure::Thread); return;
    }
    if (!workerBatch || !worker.inBatch) { workerFailure(Failure::Association); return; }
    if (worker.status.failures) { workerFailure(Failure::Association); return; }
    if (compress) {
        data[5] = 1;
        if (!worker.records.empty()) {
            auto& prior = worker.records.back();
            auto compared = prior.data; compared[5] = 1;
            if (prior.kind == kind && prior.flags == 1 && prior.batch == workerBatch &&
                prior.sampleOrdinal + prior.data[5] == workerOrdinal && compared == data && prior.data[5] < UINT32_MAX) {
                ++prior.data[5]; return;
            }
        }
    }
    if (worker.records.size() == worker.status.limit) { workerFailure(Failure::Events); return; }
    WorkerRecord row;
    row.sequence = worker.records.size() + 1; row.epoch = worker.status.epoch;
    row.batch = workerBatch; row.sampleOrdinal = workerOrdinal;
    row.cpuPublicationCycle = cpuPublication.load(std::memory_order_relaxed);
    row.relatedFeed = feedPublication.load(std::memory_order_relaxed);
    row.kind = kind; row.flags = compress ? 1 : 0; row.data = data;
    worker.records.push_back(row); worker.status.events = worker.records.size();
}
}


bool idle() { return History::ownerThread() && !depth && !medium && !feeding; }
bool active() { return History::active() && enabled; }
bool limits(uint32_t flags, Limits* out) {
    if (!out || flags & ~3u || ((flags & 2) && !(flags & 1)) || !idle() || g_system->running() || g_system->quitting() || History::active()) return false;
    SPU::Environment candidate;
    const auto nextDisc = g_emulator->m_cdrom->getIso();
    if (!g_emulator->m_cdrom->historyInitialized() || !g_emulator->m_spu->captureEnvironment(candidate) ||
        !nextDisc || !nextDisc->historyPrepare()) return false;
    const uint64_t floor = 5628ull + candidate.backend.size() + candidate.device.size() + 2ull * nextDisc->historyBytes() + ((flags & 2) ? 96 : 0);
    if (floor > 8388608) return false;
    *out = {}; out->minimumBoundaryEvents = (flags & 2) ? 47 : 45; out->minimumBoundaryBytes = floor;
    return true;
}
bool prepare(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags) {
    Limits bound;
    if (!limits(flags, &bound) || events < bound.minimumBoundaryEvents || events > bound.eventCeiling ||
        bytes < bound.minimumBoundaryBytes || bytes > bound.payloadCeiling ||
        ((flags & 2) ? (!workerEvents || workerEvents > bound.workerEventCeiling) : workerEvents != 0)) return false;
    SPU::Environment candidate;
    if (!g_emulator->m_spu->captureEnvironment(candidate)) return false;
    const auto nextClock = g_emulator->m_psxClockSpeed;
    if (!nextClock || nextClock > 100000000) return false;
    std::array<uint32_t, 11> nextScales{};
    for (unsigned n = 0; n < nextScales.size(); ++n) {
        const float scale = g_emulator->m_cpu->m_scheduleScales[n];
        if (!std::isfinite(scale) || scale <= 0 || scale > 16) return false;
        nextScales[n] = std::bit_cast<uint32_t>(scale);
    }
    std::vector<WorkerRecord> records;
    try { records.reserve(workerEvents); } catch (...) { return false; }
    { std::lock_guard guard(worker.mutex); worker.records.swap(records); }
    environment = std::move(candidate); clock = nextClock; scales = nextScales;
    disc = g_emulator->m_cdrom->getIso(); preparedFlags = flags; preparedWorkerLimit = workerEvents;
    return true;
}
void configure(bool value, uint64_t next, bool audio, uint64_t audioEpoch) {
    if (!idle()) { History::fail(Failure::Association); return; }
    enabled = value; generation = next; serial = 0;
    joinAudio = audio; audioGeneration = audioEpoch;
    backingSelection = 0; currentStream = latestMutation = latestSubmission = currentPass = 0;
    cdScheduleOwners.fill(0); fifoOrigins.reset(); fifoShadowReady = false; observingDma = false;
    cpuPublication.store(g_emulator->m_cpu->m_regs.cycle, std::memory_order_relaxed); feedPublication.store(0, std::memory_order_relaxed);
    {
        std::lock_guard guard(worker.mutex);
        worker.enabled = value && (preparedFlags & 2); worker.freeze = false; worker.freezeBatch = 0;
        worker.status = {}; worker.status.epoch = next; worker.status.state = worker.enabled ? 1 : 0;
        worker.status.limit = worker.enabled ? preparedWorkerLimit : 0; worker.thread = {};
        worker.first = worker.last = worker.pendingEpoch = 0; worker.pendingCount = 0;
        worker.pendingFlags = g_emulator->m_cpu->m_regs.spuInterrupt.load() ? 1 : 0;
        if (worker.enabled && worker.inBatch) workerFailure(Failure::Association);
    }
    if (value) {
        std::array<uint8_t, 156 + 2048> payload{};
        for (unsigned n = 0; n < environment.settings.size(); ++n)
            for (unsigned b = 0; b < 8; ++b) payload[n*8+b] = uint64_t(environment.settings[n]) >> (b*8);
        for (unsigned n = 0; n < scales.size(); ++n)
            for (unsigned b = 0; b < 4; ++b) payload[112+n*4+b] = scales[n] >> (b*8);
        const auto backend = uint32_t(environment.backend.size()), device = uint32_t(environment.device.size());
        std::copy(environment.backend.begin(), environment.backend.end(), payload.begin()+156);
        std::copy(environment.device.begin(), environment.device.end(), payload.begin()+156+backend);
        History::emit(Kind::CdContext, 2, {3, clock, 14, 11, backend, device,
                      uint32_t(environment.initialized), uint32_t(environment.stream)},
                      0, payload.data(), 156+backend+device, 0);
        g_emulator->m_cdrom->historyReset();
        boundary(0);
        if (preparedFlags & 2) {
            std::array<uint8_t, 48> status{};
            little(status.data(), next, 8); little(status.data()+24, 3, 4); little(status.data()+28, 1, 4);
            little(status.data()+36, preparedWorkerLimit, 4);
            History::emit(Kind::CdTransition, 16, {}, 0, status.data(), status.size());
        }
    } else {
        disc.reset();
    }
}
void boundary(uint32_t phase) {
    if (!active()) return;
    const auto current = g_emulator->m_cdrom->getIso();
    if (!current || disc.owner_before(current) || current.owner_before(disc)) {
        History::fail(Failure::Discontinuity); return;
    }
    current->history(phase);
}
uint64_t epoch() { return active() ? generation : 0; }
uint64_t audioEpoch() { return active() ? audioGeneration : 0; }
bool joined() { return active() && joinAudio; }
uint64_t operation() { return active() && depth ? stack[0].id : 0; }
uint64_t access() { return active() && depth ? stack[depth - 1].id : 0; }
void predicate(Predicate kind, std::array<uint32_t, 8> values, const void* payload, uint32_t length) {
    if (!active()) return;
    if (!access()) { History::fail(Failure::Association); return; }
    History::emit(Kind::CdPredicate, uint32_t(kind), values, operation(), payload, length, access());
}
void invalidate() {
    if (active()) History::fail(Failure::Discontinuity);
}
bool validate() {
    if (!active()) return true;
    const auto current = g_emulator->m_cdrom->getIso();
    bool valid = !g_emulator->m_cpu->isDynarec() && !g_emulator->m_mem->msanInitialized() &&
        g_emulator->m_psxClockSpeed == clock && g_emulator->m_spu->checkEnvironment(environment) &&
        current && !disc.owner_before(current) && !current.owner_before(disc) && current->historyValidate();
    for (unsigned n = 0; n < scales.size(); ++n)
        valid = valid && std::bit_cast<uint32_t>(g_emulator->m_cpu->m_scheduleScales[n]) == scales[n];
    if (!valid) { History::fail(Failure::Discontinuity); return false; }
    if (!joinAudio) return true;
    const auto identity = g_emulator->m_spu->captureIdentity();
    if (identity[0] == 1 && identity[1] == audioGeneration) return true;
    History::fail(Failure::Discontinuity);
    return false;
}

Media::Media(uint32_t kind, uint32_t msf, uint32_t lba, uint64_t prior) : m_kind(kind) {
    if (!active()) return;
    if (!access() || medium || serial == UINT64_MAX) { History::fail(Failure::Association); return; }
    m_id = ++serial; m_epoch = generation; medium = m_id; mediaOutcomes = 0;
    History::emit(Kind::CdMedia, 0, {kind, msf, lba, uint32_t(prior), uint32_t(prior >> 32)},
                  access(), nullptr, 0, m_id);
}
void Media::result(bool success, uint32_t disposition) {
    if (!m_id || !active()) return;
    if (m_epoch != generation || medium != m_id || m_finished) { History::fail(Failure::Association); return; }
    if (disposition > 2 || (m_kind == 2 && disposition != 0) ||
        mediaOutcomes != (disposition == 0 ? 1u : 0u)) History::fail(Failure::Association);
    History::emit(Kind::CdMedia, 1, {m_kind, uint32_t(success), disposition}, access(), nullptr, 0, m_id);
    m_finished = true;
}
Media::~Media() {
    if (!m_id || !History::ownerThread()) return;
    if (m_epoch != generation || medium != m_id || !m_finished) History::fail(Failure::Association);
    if (medium == m_id) medium = 0;
}
void mediaQuery(uint32_t kind, uint32_t argument, uint32_t result, const void* payload, uint32_t length) {
    if (!active() || !access()) return;
    History::emit(Kind::CdMedia, 6, {kind, argument, result}, access(), payload, length, operation());
}
uint64_t backing() { return active() ? backingSelection : 0; }
void mediaOutcome(uint32_t reason, int64_t count, uint32_t track, uint32_t file,
                  uint32_t sector, uint32_t flags, uint32_t base) {
    if (!active()) return;
    if (!medium || !access()) { History::fail(Failure::Association); return; }
    if (mediaOutcomes) { History::fail(Failure::Association); return; }
    mediaOutcomes = 1;
    if (flags & 4) backingSelection = medium;
    const auto bits = uint64_t(count);
    History::emit(Kind::CdMedia, 2, {reason, uint32_t(bits), uint32_t(bits >> 32), track, file, sector, flags, base},
                  access(), nullptr, 0, medium);
}

Feed::Feed(bool cdda, uint64_t media, uint64_t mutation, uint64_t stream,
           uint32_t rate, uint32_t frames, uint32_t stereo) {
    if (!active()) return;
    if (!access() || feeding || serial == UINT64_MAX) { History::fail(Failure::Association); return; }
    m_id = ++serial; m_epoch = generation; feeding = m_id; feedPublication.store(m_id, std::memory_order_relaxed); feedFinished = false; feedCdda = cdda;
    std::array<uint8_t, 24> payload{};
    const std::array<uint64_t, 3> ids = {media, mutation, stream};
    for (unsigned n = 0; n < ids.size(); ++n)
        for (unsigned b = 0; b < 8; ++b) payload[n*8+b] = ids[n] >> (b*8);
    History::emit(Kind::CdAudio, 0, {uint32_t(cdda), rate, frames, stereo}, access(),
                  payload.data(), payload.size(), m_id);
}
Feed::~Feed() {
    if (!m_id || !History::ownerThread()) return;
    if (m_epoch != generation || feeding != m_id || !feedFinished) History::fail(Failure::Association);
    if (feeding == m_id) feeding = 0;
}
bool feedActive() { return active() && feeding && !feedFinished; }
void feedOutcome(uint32_t reason, uint32_t rate, uint32_t sourceFrames, uint32_t stereo,
                 uint32_t outputFrames, bool cdda, uint64_t audioState, uint64_t epoch, uint64_t event) {
    if (!active()) return;
    if (!feeding || feedFinished || cdda != feedCdda || reason < 1 || reason > 7 || audioState > 2) {
        History::fail(Failure::Association); return;
    }
    feedFinished = true;
    const bool submitted = reason >= 6;
    const uint32_t disposition = !submitted ? 0 : (audioState != 1 ? 1 : (event ? 2 : 3));
    std::array<uint8_t, 16> payload{};
    const std::array<uint64_t, 2> ids = {epoch, access()};
    for (unsigned n = 0; n < ids.size(); ++n)
        for (unsigned b = 0; b < 8; ++b) payload[n*8+b] = ids[n] >> (b*8);
    History::emit(Kind::CdAudio, 1, {reason, rate, sourceFrames, stereo, outputFrames,
                  uint32_t(audioState), disposition, uint32_t(cdda)}, event, payload.data(), payload.size(), feeding);
    if (joinAudio && (audioState != 1 || epoch != audioGeneration || (submitted && !event)))
        History::fail(Failure::Association);
}

Scope::Scope(Operation kind, uint32_t address, uint32_t width, uint32_t value)
    : m_kind(kind), m_address(address), m_width(width), m_value(value) {
    if (kind == Operation::CpuRead || kind == Operation::CpuWrite) {
        const auto physical = address & 0x1fffffff;
        if (physical < 0x1f801800 || physical >= 0x1f801804) return;
    }
    if (!History::ownerThread()) return;
    if (depth == stack.size()) { History::fail(Failure::Bounds); return; }
    const bool recording = active();
    if (recording) {
        if (g_emulator->m_cpu->isDynarec() || g_emulator->m_mem->msanInitialized())
            History::fail(Failure::Unsupported);
        if ((depth && !nested(stack[depth - 1].kind, kind)) ||
            (!depth && (kind == Operation::HandlerRead || kind == Operation::HandlerWrite)))
            History::fail(Failure::Association);
        if (isPort(kind) && width != 8) History::fail(Failure::Unsupported);
        if (serial == UINT64_MAX) { History::fail(Failure::Bounds); return; }
        m_id = ++serial; m_parent = depth ? stack[depth - 1].id : 0; m_epoch = generation;
        validate();
    }
    stack[depth++] = {kind, m_id}; m_entered = true;
    if (recording) { cpuPublication.store(g_emulator->m_cpu->m_regs.cycle, std::memory_order_relaxed); emit(0); }
}
Scope::~Scope() {
    if (!m_entered || !History::ownerThread()) return;
    if (!depth || stack[depth - 1].kind != m_kind || stack[depth - 1].id != m_id) {
        History::fail(Failure::Association); return;
    }
    if (m_id && active()) {
        if (m_epoch != generation || (isRead(m_kind) && !m_returned)) History::fail(Failure::Association);
        validate(); emit(1);
    }
    --depth;
}
void Scope::emit(uint32_t phase) {
    if (!active()) return;
    if (m_kind == Operation::IsoScan) g_emulator->m_cdrom->historyIsoIdentity(phase);
    History::emit(isPort(m_kind) ? Kind::CdPort : Kind::CdTransition, phase,
        {uint32_t(m_kind), m_address, m_width, m_value, g_emulator->m_cpu->m_regs.pc,
         uint32_t(m_returned), uint32_t(depth), 0}, m_parent, g_emulator->m_cdrom->historyContext().data(), 384, m_id);
}

uint64_t nextIdentity() {
    if (!active()) return 0;
    if (serial == UINT64_MAX) { History::fail(Failure::Bounds); return 0; }
    return ++serial;
}
std::array<uint64_t, 8> identities() {
    return {operation(), access(), currentStream, latestMutation, medium, feeding, latestSubmission, currentPass};
}
std::array<uint64_t, 4> scheduleOwners() { return cdScheduleOwners; }
void submission() { if (active()) latestSubmission = nextIdentity(); }
void stream(uint32_t type) { if (active()) currentStream = type ? nextIdentity() : 0; }
void schedulerPass(uint32_t mask, uint64_t lowest) {
    if (!active()) return;
    cpuPublication.store(g_emulator->m_cpu->m_regs.cycle, std::memory_order_relaxed);
    currentPass = nextIdentity();
    History::emit(Kind::CdSchedule, 3, {mask, g_emulator->m_cpu->m_regs.scheduleMask}, lowest, nullptr, 0, currentPass);
}
void schedulerTest(uint32_t slot, uint32_t mask, uint32_t live, uint64_t target, bool dispatch) {
    if (!active()) return;
    const int index = cdSlot(slot);
    if (index < 0) return;
    std::array<uint8_t, 48> payload{};
    little(payload.data(), currentPass, 8); little(payload.data()+8, cdScheduleOwners[index], 8);
    little(payload.data()+16, target, 8); little(payload.data()+24, target, 8);
    little(payload.data()+32, mask, 4); little(payload.data()+36, live, 4);
    little(payload.data()+40, std::bit_cast<uint32_t>(g_emulator->m_cpu->m_scheduleScales[slot]), 4);
    History::emit(Kind::CdSchedule, dispatch ? 5 : 4, {slot, uint32_t(dispatch)}, access(), payload.data(), payload.size(), cdScheduleOwners[index]);
    if (dispatch) cdScheduleOwners[index] = 0;
}
void schedulerChange(uint32_t slot, uint32_t mask, uint64_t oldTarget, uint64_t newTarget, uint32_t scale, bool cancel) {
    if (!active()) return;
    const int index = cdSlot(slot);
    if (index < 0) return;
    const uint64_t previous = cdScheduleOwners[index];
    const uint64_t next = cancel ? 0 : nextIdentity();
    std::array<uint8_t, 48> payload{};
    little(payload.data(), currentPass, 8); little(payload.data()+8, previous, 8);
    little(payload.data()+16, oldTarget, 8); little(payload.data()+24, newTarget, 8);
    little(payload.data()+32, mask, 4); little(payload.data()+36, cancel ? mask & ~(1u << slot) : mask | (1u << slot), 4);
    little(payload.data()+40, scale, 4);
    History::emit(Kind::CdSchedule, cancel ? 2 : 1, {slot, uint32_t(cancel)}, access(), payload.data(), payload.size(), next);
    cdScheduleOwners[index] = next;
}
void dataBoundary(uint32_t phase, const void* bytes, uint32_t length) {
    if (!active()) return;
    if (!bytes || length != fifoShadow.size() || phase > 1) { History::fail(Failure::Bounds); return; }
    if (!phase) { std::memcpy(fifoShadow.data(), bytes, length); fifoShadowReady = true; }
    else if (!fifoShadowReady || std::memcmp(fifoShadow.data(), bytes, length)) History::fail(Failure::Association);
}
void dataMutation(uint32_t buffer, uint32_t offset, uint32_t length, const void* bytes, uint64_t media) {
    if (!active()) return;
    if (!length || !bytes || offset > (buffer >= 6 ? 16384u : Origins::Size) || length > (buffer >= 6 ? 16384u : Origins::Size)-offset) { History::fail(Failure::Bounds); return; }
    latestMutation = nextIdentity();
    if (buffer == 1) {
        if (!fifoShadowReady || !fifoOrigins.assign(offset, length, latestMutation)) { History::fail(Failure::Association); return; }
        std::memcpy(fifoShadow.data()+offset, bytes, length);
    }
    History::emit(Kind::CdBuffer, 2, {buffer, offset, length}, media, bytes, length, latestMutation);
}
void consume(uint32_t offset, uint32_t length, const void* bytes, uint32_t address, bool dma) {
    if (!active()) return;
    if (offset > Origins::Size || length > Origins::Size-offset || (length && !bytes)) { History::fail(Failure::Bounds); return; }
    if (!fifoShadowReady || (length && std::memcmp(bytes, fifoShadow.data()+offset, length))) { History::fail(Failure::Association); return; }
    std::array<uint8_t, 40016> payload{};
    uint32_t runs = 0, cursor = 32;
    for (uint32_t n = 0; n < length;) {
        uint64_t origin = 0; fifoOrigins.get(offset+n, origin);
        uint32_t end = n+1;
        while (end < length) { uint64_t next = 0; fifoOrigins.get(offset+end, next); if (next != origin) break; ++end; }
        little(payload.data()+cursor, n, 4); little(payload.data()+cursor+4, end-n, 4);
        little(payload.data()+cursor+8, origin, 8); cursor += 16; ++runs; n = end;
    }
    little(payload.data(), 3, 4); little(payload.data()+4, dma ? 1 : 0, 4);
    little(payload.data()+8, offset, 4); little(payload.data()+12, length, 4);
    little(payload.data()+16, address, 4); little(payload.data()+20, runs, 4);
    little(payload.data()+24, currentStream, 8);
    if (length) std::memcpy(payload.data()+cursor, bytes, length);
    History::emit(Kind::CdData, dma ? 1 : 0, {offset, length, address, runs}, access(), payload.data(), cursor+length, operation());
}
bool dmaActive() { return History::active() && observingDma; }
bool isoScan() { return active() && depth && stack[0].kind == Operation::IsoScan; }
void dmaBegin(uint32_t address, uint32_t offset, uint32_t length) {
    if (!History::active()) return;
    if (observingDma) { History::fail(Failure::Association); return; }
    observingDma = true; dmaAddress = address; dmaOffset = offset; dmaLength = length; dmaReceived = 0;
}
void dmaEnd(uint32_t returned) {
    if (!observingDma) return;
    if (active()) History::emit(Kind::CdData, 4, {dmaAddress, dmaOffset, dmaLength, returned, dmaReceived}, access(), nullptr, 0, operation());
    if (dmaReceived != returned) History::fail(Failure::Association);
    observingDma = false;
}
void dmaReceipt(uint32_t address, uint32_t length, const void* source, uint32_t copied, uint32_t destination, uint32_t destinationOffset) {
    if (!History::active() || !observingDma) return;
    if (length > dmaLength-dmaReceived || address != uint32_t(dmaAddress+dmaReceived) || copied > length || (copied && !source)) {
        History::fail(Failure::Bounds); return;
    }
    if (active()) History::emit(Kind::CdData, 3, {address, dmaOffset+dmaReceived, length, copied, length-copied, destination, destinationOffset}, access(), nullptr, 0, operation());
    if (copied) History::emit(Kind::DmaData, 3, {address, dmaOffset+dmaReceived, copied, 0, destinationOffset, destination}, 0, source, copied);
    dmaReceived += length;
}

WorkerBatch::WorkerBatch() {
    std::lock_guard guard(worker.mutex);
    if (worker.inBatch) { if (worker.enabled) workerFailure(Failure::Thread); return; }
    if (worker.batch == UINT64_MAX) { if (worker.enabled) workerFailure(Failure::Bounds); return; }
    worker.inBatch = true; workerBatch = ++worker.batch; workerOrdinal = 0; m_observed = true;
    if (worker.enabled && !worker.freeze) {
        if (worker.thread == std::thread::id{}) worker.thread = std::this_thread::get_id();
        if (!worker.status.beginBatch) worker.status.beginBatch = workerBatch;
        workerAppend(1, {});
    }
}
WorkerBatch::~WorkerBatch() { finish(); }
void WorkerBatch::finish() {
    if (!m_observed) return;
    std::lock_guard guard(worker.mutex);
    workerAppend(6, {});
    if (worker.enabled && (!worker.freeze || workerBatch == worker.freezeBatch)) worker.status.endBatch = workerBatch;
    worker.inBatch = false; workerBatch = 0; m_observed = false; worker.boundary.notify_all();
}
void workerSample(uint32_t sample) { workerOrdinal = sample; }
void workerObserve(uint32_t kind, std::array<uint32_t, 6> data, bool compress) {
    std::lock_guard guard(worker.mutex); workerAppend(kind, data, compress);
}
void publishSpuInterrupt(std::atomic<bool>& flag, uint32_t kind, uint32_t channel, uint32_t address, uint32_t control, uint32_t status) {
    std::lock_guard guard(worker.mutex);
    if (worker.enabled && (!worker.freeze || workerBatch == worker.freezeBatch)) {
        const uint32_t before = worker.status.events;
        workerAppend(5, {kind, channel, address, control, uint32_t(flag.load()), status});
        if (worker.status.events != before) {
            if (!worker.pendingCount) { worker.first = worker.status.events; worker.pendingEpoch = worker.status.epoch; }
            worker.last = worker.status.events;
            if (worker.pendingCount != UINT32_MAX) ++worker.pendingCount;
            else workerFailure(Failure::Bounds);
        } else worker.pendingFlags |= 2;
    } else worker.pendingFlags |= 1;
    flag = true;
}
bool takeSpuInterrupt(std::atomic<bool>& flag) {
    std::array<uint8_t, 32> payload{};
    bool taken = false, observed = false;
    {
        std::lock_guard guard(worker.mutex);
        taken = flag.exchange(false); observed = worker.enabled && !worker.freeze;
        if (taken) {
            little(payload.data(), worker.pendingEpoch, 8); little(payload.data()+8, worker.first, 8);
            little(payload.data()+16, worker.last, 8); little(payload.data()+24, worker.pendingCount, 4);
            little(payload.data()+28, worker.pendingFlags | (worker.pendingCount > 1 ? 4 : 0), 4);
            worker.first = worker.last = worker.pendingEpoch = 0; worker.pendingCount = worker.pendingFlags = 0;
        }
    }
    if (taken && observed && active()) {
        History::emit(Kind::CdIRQ, 4, {}, 0, payload.data(), payload.size());
        if (payload[28] & 3) History::fail(Failure::Association);
    }
    return taken;
}
bool freezeWorker() {
    uint32_t failure = 0;
    {
        std::unique_lock guard(worker.mutex);
        if (!worker.enabled) return true;
        worker.freeze = true; worker.freezeBatch = worker.inBatch ? worker.batch : 0;
        worker.boundary.wait(guard, [] { return !worker.inBatch || worker.batch > worker.freezeBatch; });
        worker.status.state = 2; failure = worker.status.failures;
    }
    if (failure) History::fail(Failure::Association);
    WorkerStatus status; workerStatus(&status);
    std::array<uint8_t, 48> payload{};
    little(payload.data(), status.epoch, 8); little(payload.data()+8, status.beginBatch, 8); little(payload.data()+16, status.endBatch, 8);
    const std::array<uint32_t, 6> words{status.version, status.state, status.events, status.limit, status.failures, status.dropped};
    for (unsigned n = 0; n < words.size(); ++n) little(payload.data()+24+n*4, words[n], 4);
    History::emit(Kind::CdTransition, 17, {}, 0, payload.data(), payload.size());
    return true;
}
bool workerStatus(WorkerStatus* out) {
    if (!out || !History::ownerThread() || g_system->running() || g_system->quitting()) return false;
    std::lock_guard guard(worker.mutex); *out = worker.status; return true;
}
bool workerRecords(uint32_t first, uint32_t count, WorkerRecord* out) {
    if (!History::ownerThread() || g_system->running() || g_system->quitting()) return false;
    std::lock_guard guard(worker.mutex);
    if (worker.status.state != 2 || count > 65536 || first > worker.records.size() ||
        count > worker.records.size() - first || (count && !out)) return false;
    if (count) std::memcpy(out, worker.records.data() + first, size_t(count) * sizeof(WorkerRecord));
    return true;
}
bool workerSnapshot(WorkerSnapshot& out) {
    if (!History::ownerThread() || g_system->running() || g_system->quitting()) return false;
    std::lock_guard guard(worker.mutex);
    if (worker.status.state == 1 || worker.records.size() > 65536) return false;
    out.status = worker.status;
    if (worker.status.state == 2) out.records = worker.records;
    else out.records.clear();
    return true;
}

}
