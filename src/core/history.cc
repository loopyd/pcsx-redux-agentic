// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/history.h"

#include <memory>
#include <atomic>

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/mdec.h"
#include "core/gpu.h"
#include "core/cdrom.h"
#include "core/spu.h"
#include "core/transactions.h"
#include "core/psxcounters.h"

namespace PCSX::History {
namespace {
std::unique_ptr<Journal> storage;
std::atomic<Journal*> published{nullptr};
std::once_flag initialization;
Journal* journal() { return published.load(std::memory_order_acquire); }
std::array<uint64_t, 7> requests{};
uint64_t serial = 0;
uint64_t gpuSerial = 0;
uint32_t gpuDepth = 0;
uint64_t bufferRequest = 0;
uint64_t inputRequest = 0, pendingRequest = 0, epoch = 0;
std::array<uint64_t, 11> scheduled{};
std::array<uint64_t, 7> owners{};
std::array<bool, 7> owned{};
std::array<uint32_t, 2> reading{};

bool paused() {
    return journal() && journal()->owner() && !g_system->running() && !g_system->quitting();
}
uint64_t cycles() { return g_emulator->m_cpu->m_regs.cycle; }
bool dmaKind(Kind kind) {
    return kind == Kind::DmaStart || kind == Kind::DmaData || kind == Kind::DmaHeader ||
           kind == Kind::DmaComplete || kind == Kind::DmaIRQ || kind == Kind::DmaDeferred ||
           kind == Kind::DmaInitial || kind == Kind::DmaEnd ||
           kind == Kind::DmaDevice || kind == Kind::DmaBuffer || kind == Kind::DmaCancel || kind == Kind::DmaPeek;
}
bool recording() {
    if (!journal()) return false;
    if (!journal()->owner()) { journal()->fail(Failure::Thread); return false; }
    Status state;
    return journal()->status(state) && state.state == 1;
}
uint32_t mirror(uint32_t address) {
    const uint8_t* p = g_emulator->m_mem->m_hard + (address & 0xfffc);
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
int dmaInterrupt(uint32_t device) {
    switch (device) {
        case static_cast<unsigned>(Schedule::MDECINDMA): return 0;
        case static_cast<unsigned>(Schedule::MDECOUTDMA): return 1;
        case static_cast<unsigned>(Schedule::GPUDMA): return 2;
        case static_cast<unsigned>(Schedule::CDRDMA): return 3;
        case static_cast<unsigned>(Schedule::SPUDMA): return 4;
        case static_cast<unsigned>(Schedule::GPUOTCDMA): return 6;
        default: return -1;
    }
}
}

void initialize() {
    std::call_once(initialization, [] {
        storage = std::make_unique<Journal>();
        published.store(storage.get(), std::memory_order_release);
    });
}
bool active() { return recording(); }
StateBoundary stateBoundary() noexcept {
    auto* current = journal();
    if (!current || !current->owner()) return StateBoundary::Unavailable;
    try {
        Status state;
        if (!current->status(state)) return StateBoundary::Unavailable;
        if (state.state == 1) return StateBoundary::Recording;
        if (gpuDepth || !CD::idle()) return StateBoundary::Unsettled;
        return StateBoundary::Ready;
    } catch (...) {
        return StateBoundary::Unavailable;
    }
}
bool ownerThread() {
    if (!journal()) return false;
    if (journal()->owner()) return true;
    journal()->fail(Failure::Thread); return false;
}
static bool beginProfile(uint32_t events, uint32_t bytes, bool cd, bool audio, uint32_t workerEvents = 0, uint32_t flags = 0) {
    if (!paused() || !CD::idle() || gpuDepth || epoch == UINT64_MAX ||
        g_emulator->m_cpu->isDynarec() || g_emulator->m_mem->msanInitialized()) return false;
    const auto identity = g_emulator->m_spu->captureIdentity();
    Status before;
    if (!journal()->status(before) || before.state || (audio && identity[0] != 1)) return false;
    if (cd && !CD::prepare(events, bytes, workerEvents, flags)) return false;
    if (!journal()->begin(events, bytes, cycles())) return false;
    requests.fill(0);
    serial = 0;
    gpuSerial = 0;
    bufferRequest = 0;
    inputRequest = pendingRequest = 0;
    scheduled.fill(0);
    owners.fill(0);
    owned.fill(false);
    ++epoch;
    CD::configure(cd, epoch, audio, audio ? identity[1] : 0);
    reading = {};
    for (uint32_t n = 0; n < 7; ++n) {
        const auto* p = g_emulator->m_mem->m_hard + 0x1080 + 16 * n;
        std::array<uint32_t, 8> data{};
        for (unsigned r = 0; r < 3; ++r) {
            for (unsigned b = 0; b < 4; ++b) data[r] |= uint32_t(p[4 * r + b]) << (8 * b);
        }
        emit(Kind::DmaInitial, n, data);
    }
    const auto &cpu = g_emulator->m_cpu->m_regs;
    for (uint32_t irq = 0; irq < 11; ++irq) {
        emit(Kind::ScheduleInitial, irq, {(cpu.scheduleMask >> irq) & 1}, cpu.scheduleTargets[irq]);
    }
    emit(Kind::ScheduleInitial, UINT32_MAX, {cpu.scheduleMask}, cpu.lowestTarget);
    if (cpu.scheduleMask & ~0x7ffu) fail(Failure::Unsupported);
    emit(Kind::IRQInitial, 0, {mirror(0x1070), mirror(0x1074), cpu.CP0.n.Status, cpu.CP0.n.Cause});
    g_emulator->m_mdec->observe(0);
    g_emulator->m_counters->history();
    g_emulator->m_gpu->history(0);
    if (cd) g_emulator->m_cdrom->history(0);
    return true;
}
bool begin(uint32_t events, uint32_t bytes) { return beginProfile(events, bytes, false, false); }
bool beginCD(uint32_t events, uint32_t bytes, bool audio) { return beginProfile(events, bytes, true, audio, 0, audio ? 1 : 0); }
bool beginCDV3(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags) { return beginProfile(events, bytes, true, flags & 1, workerEvents, flags); }
bool stop() {
    if (!paused() || !recording() || !CD::idle()) return false;
    if (g_emulator->m_cpu->isDynarec() || g_emulator->m_mem->msanInitialized()) fail(Failure::Unsupported);
    g_emulator->m_gpu->history(1);
    if (CD::active()) {
        CD::validate();
        CD::freezeWorker();
        CD::boundary(1);
        g_emulator->m_cdrom->history(1);
    }
    if (gpuDepth) fail(Failure::Association);
    return journal()->stop(cycles());
}
bool clear() {
    if (!paused() || !CD::idle() || !journal()->clear()) return false;
    CD::configure(false, epoch, false, 0); return true;
}
bool status(Status* out) { return out && journal() && journal()->status(*out); }
bool records(uint32_t first, uint32_t count, Record* out) { return paused() && journal()->records(first, count, out); }
bool bytes(uint32_t offset, uint32_t length, void* out) { return paused() && journal()->bytes(offset, length, out); }
void fail(Failure reason) { if (journal()) journal()->fail(reason); }

Gpu::Gpu(GPU& gpu, GpuSource source, uint32_t origin, uint32_t words, uint32_t value)
    : m_source(source), m_origin(origin), m_words(words), m_value(value) {
    if (!journal()) return;
    if (!journal()->owner()) { fail(Failure::Thread); return; }
    if (gpuDepth == UINT32_MAX) { fail(Failure::Bounds); return; }
    ++gpuDepth;
    m_scope = true;
    if (!recording()) return;
    if (&gpu != g_emulator->m_gpu.get() || g_emulator->m_cpu->isDynarec() ||
        g_emulator->m_mem->msanInitialized()) { fail(Failure::Unsupported); return; }
    constexpr uint32_t MaxDMAWords = 0x200000u;
    constexpr uint32_t MaxChainDMAWords = 255u;
    if (((source == GpuSource::Direct || source == GpuSource::ReadDMA) && words > MaxDMAWords) ||
        (source == GpuSource::Chain && words > MaxChainDMAWords)) { fail(Failure::Bounds); return; }
    if (source == GpuSource::ReadDMA) {
        constexpr size_t ReadFifoBytesStateSlot = 1;
        constexpr uint32_t DMAWordBytes = 4;
        const uint32_t fifoBytes = gpu.historyState()[ReadFifoBytesStateSlot];
        const uint32_t fifoWords = fifoBytes / DMAWordBytes;
        if ((fifoBytes & (DMAWordBytes - 1u)) || fifoWords > words) { fail(Failure::Bounds); return; }
    }
    if (gpuSerial == UINT64_MAX) { fail(Failure::Bounds); return; }
    m_gpu = &gpu;
    m_id = ++gpuSerial;
    m_epoch = epoch;
    if (source == GpuSource::Direct || source == GpuSource::Chain || source == GpuSource::ReadDMA) {
        m_request = owned[2] ? owners[2] : requests[2];
    }
    emit(Kind::GpuInput);
    if (source == GpuSource::HostGP0 || source == GpuSource::HostGP1 || source == GpuSource::HostRead ||
        source == GpuSource::HostReplay) {
        fail(Failure::Discontinuity);
    }
}
Gpu::~Gpu() {
    if (m_scope) {
        if (!journal()->owner()) { fail(Failure::Thread); return; }
        if (!gpuDepth) { fail(Failure::Association); return; }
        --gpuDepth;
    }
    if (!m_gpu || !recording() || m_epoch != epoch) return;
    if (m_gpu != g_emulator->m_gpu.get()) { fail(Failure::Discontinuity); return; }
    emit(Kind::GpuResult);
}
void Gpu::emit(Kind kind) {
    const auto state = m_gpu->historyState();
    History::emit(kind, 2, {uint32_t(m_source), m_origin, m_words, m_value,
                  state[0], state[1], g_emulator->m_cpu->m_regs.pc, state[5]}, m_id, nullptr, 0, m_request);
}

void emit(Kind kind, uint32_t device, std::array<uint32_t, 8> data,
          uint64_t related, const void* payload, uint32_t length, uint64_t request) {
    if (!recording()) return;
    Record event;
    event.kind = static_cast<uint32_t>(kind);
    event.device = device;
    event.data = data;
    event.related = related;
    if (dmaKind(kind)) {
        if (device >= requests.size()) { fail(Failure::Bounds); return; }
        event.request = owned[device] ? owners[device] : requests[device];
        if (kind == Kind::DmaComplete || kind == Kind::DmaIRQ) event.related = requests[device];
    } else if (kind == Kind::Schedule) {
        if (device >= scheduled.size()) { fail(Failure::Bounds); return; }
        int channel = dmaInterrupt(device);
        if (channel >= 0) event.request = owned[channel] ? owners[channel] : requests[channel];
        event.data[4] = uint32_t(scheduled[device]);
        event.data[5] = uint32_t(scheduled[device] >> 32);
        scheduled[device] = event.request;
    } else if (kind == Kind::Dispatch) {
        if (device >= scheduled.size()) { fail(Failure::Bounds); return; }
        event.request = scheduled[device];
        scheduled[device] = 0;
    }
    if (request != UINT64_MAX) event.request = request;
    journal()->append(event, cycles, payload, length);
}

Register::Register(uint32_t address, uint32_t value, uint32_t width, bool read)
    : m_address(address), m_value(value), m_width(width), m_read(read) {
    if (!recording()) return;
    uint32_t physical = address & 0x1fffffff;
    if (!((physical >= 0x1f801070 && physical < 0x1f801100) ||
          (physical >= 0x1f801100 && physical < 0x1f80112c))) return;
    m_observed = true;
    m_previous = reading;
    if (read) reading = {address, width};
    emit(Kind::RegisterAttempt, physical, {address, value, width, mirror(address), uint32_t(read)});
    if (!read && physical == 0x1f8010d8 && (value & 0x01000000)) fail(Failure::Unsupported);
}
Register::~Register() {
    if (m_observed) {
        emit(Kind::RegisterResult, m_address & 0x1fffffff,
             {m_address, m_value, m_width, mirror(m_address), uint32_t(m_read)});
        if (m_read) reading = m_previous;
    }
}
Completion::Completion(uint32_t channel) : m_channel(channel) {
    if (!recording()) return;
    if (channel >= requests.size()) { fail(Failure::Bounds); return; }
    m_observed = true;
    emit(Kind::DmaIRQ, channel, {0, mirror(0x10f4)});
}
Completion::~Completion() {
    if (!m_observed || !recording()) return;
    emit(Kind::DmaIRQ, m_channel, {1, mirror(0x10f4)});
    requests[m_channel] = 0;
}

void start(uint32_t channel, uint32_t original, uint32_t normalized, uint32_t bcr,
           uint32_t chcr, uint32_t previous, uint32_t enabled) {
    if (!recording()) return;
    if (channel >= requests.size()) { fail(Failure::Bounds); return; }
    uint64_t prior = requests[channel];
    requests[channel] = ++serial;
    emit(Kind::DmaStart, channel, {original, normalized, bcr, chcr, previous, enabled}, prior,
         nullptr, 0, requests[channel]);
    bool supported = false;
    switch (channel) {
        case 0: supported = chcr == 0x01000201; break;
        case 1: supported = chcr == 0x01000200; break;
        case 2: supported = chcr == 0x01000200 || chcr == 0x01000201 || chcr == 0x01000401; break;
        case 3: supported = chcr == 0x11000000 || chcr == 0x11400100; break;
        case 4: supported = chcr == 0x01000200 || chcr == 0x01000201; break;
        case 6: supported = chcr == 0x11000002; break;
    }
    if (!supported) fail(Failure::Unsupported);
}

void scalar(uint32_t channel, uint32_t address, uint32_t destination,
            uint32_t value, uint32_t width, uint32_t direction) {
    if (!recording()) return;
    if (width != 1 && width != 2 && width != 4) { fail(Failure::Bounds); return; }
    uint8_t payload[4];
    for (uint32_t b = 0; b < width; ++b) payload[b] = value >> (8 * b);
    emit(Kind::DmaData, channel, {address, destination, width, direction}, 0, payload, width);
}

void span(uint32_t channel, uint32_t address, const void* data, uint32_t length, uint32_t direction, uint64_t related) {
    if (!recording()) return;
    uintptr_t first = reinterpret_cast<uintptr_t>(g_emulator->m_mem->m_wram);
    uintptr_t pointer = reinterpret_cast<uintptr_t>(data);
    if (pointer < first || pointer - first > 0x800000 || length > 0x800000 - (pointer - first)) {
        fail(Failure::Bounds);
        return;
    }
    emit(Kind::DmaData, channel, {address, 0, length, direction}, related, data, length);
}

uint64_t input() { return recording() ? inputRequest : 0; }
void retain() {
    if (recording()) inputRequest = requests[0];
}
void inputScalar(uint32_t address, uint16_t value, bool peek) {
    if (!recording()) return;
    uint8_t payload[2] = {uint8_t(value), uint8_t(value >> 8)};
    emit(peek ? Kind::DmaPeek : Kind::DmaData, 0, {address, 0, 2, 1}, 0, payload, 2, inputRequest);
}
void defer(uint32_t address, uint32_t bcr, uint32_t chcr) {
    if (!recording()) return;
    uint64_t previous = pendingRequest;
    pendingRequest = requests[1];
    emit(Kind::DmaDeferred, 1, {address, bcr, chcr}, previous);
}
uint64_t pending() { return recording() ? pendingRequest : 0; }
void release() { if (recording()) pendingRequest = 0; }
uint64_t buffer() { return recording() ? bufferRequest : 0; }
void buffer(const void* data, uint32_t length) {
    if (!recording()) return;
    if (!data || (length != 512 && length != 768)) { fail(Failure::Bounds); return; }
    bufferRequest = inputRequest;
    emit(Kind::DmaBuffer, 1, {length}, bufferRequest, data, length);
}
void cancel() {
    if (!recording()) return;
    emit(Kind::DmaCancel, 1, {}, bufferRequest);
    bufferRequest = 0;
    pendingRequest = 0;
}
uint64_t dmaRequest(uint32_t channel) {
    if (!recording()) return 0;
    if (channel >= requests.size()) { fail(Failure::Bounds); return 0; }
    return owned[channel] ? owners[channel] : requests[channel];
}
void timerRead(uint32_t index, uint32_t selector, uint32_t value, uint32_t after) {
    if (!recording()) return;
    emit(Kind::TimerRead, index, {selector, value, after, reading[0], reading[1]});
}

Owner::Owner(uint32_t channel, uint64_t request) : m_channel(channel) {
    if (!recording()) return;
    if (channel >= owners.size()) { fail(Failure::Bounds); return; }
    m_observed = true;
    m_epoch = epoch;
    m_previous = owners[channel];
    m_prior = owned[channel];
    owners[channel] = request;
    owned[channel] = true;
}
Owner::~Owner() {
    if (!m_observed || !recording() || m_epoch != epoch) return;
    owners[m_channel] = m_previous;
    owned[m_channel] = m_prior;
}
Dispatch::Dispatch(uint32_t interrupt, uint64_t target) {
    if (!recording()) return;
    if (interrupt >= scheduled.size()) { fail(Failure::Bounds); return; }
    int channel = dmaInterrupt(interrupt);
    if (channel >= 0) m_owner.emplace(channel, scheduled[interrupt]);
    emit(Kind::Dispatch, interrupt, {}, target);
}

}
