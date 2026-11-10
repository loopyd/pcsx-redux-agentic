// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace PCSX::SPU {

class Capture {
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
  public:
    static constexpr size_t VOICE_COUNT = 24;
    enum Failure : uint32_t {
        Frames = 1, Blocks = 2, Events = 4, Thread = 8, Bounds = 16,
        Clock = 32, Discontinuity = 64, StreamingLoss = 128, Output = 256, Workers = 512, Gains = 1024,
        EnvelopeInputs = 2048, Scheduling = 4096,
    };
    enum class Kind : uint32_t { Access = 1, Write = 2, Feed = 3, Read = 4, Result = 5, Return = 6, FeedWait = 7 };
    enum BlockFlag : uint32_t { Mono = 1, Muted = 2, Submitted = 4, ValidBlockFlags = 7 };
    struct Block {
        uint64_t lastPublishedCpuCycle = 0;
        uint64_t voiceOffset = 0;
        uint32_t offset = 0, frames = 0, voices = 0, disc = 0, flags = 0;
    };
    struct Event {
        uint64_t cycle = 0, frame = 0, parent = 0;
        uint32_t kind = 0, address = 0, value = 0, width = 0, detail = 0, flags = 0;
    };
    struct Worker {
        uint64_t voiceOffset = 0, enqueueOffset = 0;
        uint32_t frames = 0, keyOnMask = 0, enqueueFrames = 0;
        std::array<int32_t, VOICE_COUNT> preSampleSteps{};
        std::array<int32_t, VOICE_COUNT> stopSamples{};
        int32_t firstReverbSample = -1;
    };
    struct Gain {
        uint64_t voiceOffset = 0;
        uint32_t voice = 0;
        int32_t left = 0, right = 0;
    };
    struct EnvelopeInput {
        uint64_t voiceOffset = 0;
        uint32_t voice = 0;
        int32_t phase = 0, rate = 0, exponential = 0, increasing = 0, sustainLevel = -1;
    };
    enum class ScheduleKind : uint32_t {
        Producer = 1, EnqueueBegin = 2, EnqueueEnd = 3, Dequeue = 4,
        Master = 5, Cpu = 6, WaitBegin = 7, WaitEnd = 8,
    };
    struct Schedule {
        ScheduleKind kind{};
        uint64_t cycle = 0, previousCycle = 0, voiceOffset = 0, waitNanoseconds = 0;
        uint32_t frames = 0, before = 0, after = 0, target = 0, detail = 0, flags = 0, source = 0;
    };
    struct Status {
        uint64_t beginCycle = 0, endCycle = 0, droppedFrames = 0, streamingDropped = 0;
        uint32_t version = 10, state = 0, frames = 0, blocks = 0, events = 0;
        uint32_t frameLimit = 0, blockLimit = 0, eventLimit = 0;
        uint32_t failures = 0, firstFailure = 0, droppedEvents = 0;
        uint32_t workers = 0, workerLimit = 0, droppedWorkers = 0;
        uint32_t gains = 0, gainLimit = 0, droppedGains = 0;
        uint32_t envelopeInputs = 0, envelopeInputLimit = 0, droppedEnvelopeInputs = 0;
        uint32_t scheduling = 0, schedulingLimit = 0, droppedScheduling = 0;
    };
    struct Snapshot {
        Status status;
        uint64_t epoch = 0;
        std::vector<Event> events;
    };
    static constexpr uint32_t MAX_FRAMES = 44100 * 60;
    static constexpr uint32_t MAX_RECORDS = 65536;
    static constexpr uint32_t MAX_BATCH = 1024;
    static constexpr uint32_t MAX_CHUNK = 2048;
    struct Limits {
        uint32_t frames = 0, blocks = 0, events = 0;
        bool valid() const {
            return frames && frames <= MAX_FRAMES && blocks && blocks <= MAX_RECORDS &&
                   events && events <= MAX_RECORDS;
        }
    };

    Capture() : m_owner(std::this_thread::get_id()) {}
    bool ownerThread() const { return std::this_thread::get_id() == m_owner; }
    bool active() const { return (m_lifecycle.load(std::memory_order_acquire) & STATE_MASK) == 1; }
    std::array<uint64_t, 2> identity() {
        if (!owner()) return {};
        const auto value = m_lifecycle.load(std::memory_order_acquire);
        return {value & STATE_MASK, value >> EPOCH_SHIFT};
    }

    bool begin(uint32_t frames, uint32_t blocks, uint32_t events, uint64_t cycle) {
        if (!owner() || (m_lifecycle.load() & STATE_MASK) != 0 || !frames || frames > MAX_FRAMES ||
            !blocks || blocks > MAX_RECORDS || !events || events > MAX_RECORDS || m_epoch == MAX_EPOCH) return false;
        std::lock_guard lock(m_workerMutex);
        std::lock_guard scheduleLock(m_scheduleMutex);
        try {
            std::vector<float> pcm(size_t(frames) * 2);
            std::vector<Block> records(blocks);
            std::vector<Event> writes(events);
            std::vector<Worker> workers(events);
            std::vector<Gain> gains(events);
            std::vector<EnvelopeInput> envelopeInputs(events);
            std::vector<Schedule> scheduling(events);
            m_pcm.swap(pcm); m_blocks.swap(records); m_events.swap(writes);
            m_workers.swap(workers);
            m_gains.swap(gains);
            m_envelopeInputs.swap(envelopeInputs);
            m_scheduling.swap(scheduling);
        } catch (...) { return false; }
        m_status = {};
        m_status.beginCycle = m_status.endCycle = cycle;
        m_status.frameLimit = frames; m_status.blockLimit = blocks; m_status.eventLimit = events;
        m_cycle.store(cycle); m_frame.store(0);
        m_lastCycle = cycle;
        m_workerCount = m_workerQueued = m_droppedWorkers = 0;
        m_gainCount = m_droppedGains = m_gainSeen = 0;
        m_envelopeInputCount = m_droppedEnvelopeInputs = m_envelopeInputSeen = 0;
        m_workerThread = {};
        m_scheduleCount = m_droppedScheduling = 0;
        m_scheduleThreads = {};
        m_lifecycle.store((++m_epoch << EPOCH_SHIFT) | 1, std::memory_order_release);
        return true;
    }

    bool stop(uint64_t cycle) {
        if (!owner() || !active()) return false;
        publish(cycle);
        std::lock_guard lock(m_workerMutex);
        std::lock_guard scheduleLock(m_scheduleMutex);
        m_status.endCycle = cycle;
        auto state = m_lifecycle.load();
        while (!m_lifecycle.compare_exchange_weak(state, (state & ~STATE_MASK) | 2)) {}
        return true;
    }
    bool clear() {
        if (!owner() || active()) return false;
        std::lock_guard lock(m_workerMutex);
        std::lock_guard scheduleLock(m_scheduleMutex);
        std::vector<float>().swap(m_pcm);
        std::vector<Block>().swap(m_blocks);
        std::vector<Event>().swap(m_events);
        std::vector<Worker>().swap(m_workers);
        std::vector<Gain>().swap(m_gains);
        std::vector<EnvelopeInput>().swap(m_envelopeInputs);
        std::vector<Schedule>().swap(m_scheduling);
        m_scheduleCount = m_droppedScheduling = 0;
        m_workerCount = m_workerQueued = m_droppedWorkers = 0;
        m_gainCount = m_droppedGains = m_gainSeen = 0;
        m_envelopeInputCount = m_droppedEnvelopeInputs = m_envelopeInputSeen = 0;
        m_status = {};
        m_cycle.store(0); m_frame.store(0);
        m_lifecycle.store(m_epoch << EPOCH_SHIFT, std::memory_order_release);
        return true;
    }
    bool status(Status& result) {
        if (!owner()) return false;
        std::lock_guard lock(m_workerMutex);
        std::lock_guard scheduleLock(m_scheduleMutex);
        result = m_status;
        result.workers = m_workerCount;
        result.workerLimit = m_workers.size();
        result.droppedWorkers = m_droppedWorkers;
        result.gains = m_gainCount;
        result.gainLimit = m_gains.size();
        result.droppedGains = m_droppedGains;
        result.envelopeInputs = m_envelopeInputCount;
        result.envelopeInputLimit = m_envelopeInputs.size();
        result.droppedEnvelopeInputs = m_droppedEnvelopeInputs;
        result.scheduling = m_scheduleCount;
        result.schedulingLimit = m_scheduling.size();
        result.droppedScheduling = m_droppedScheduling;
        const auto state = m_lifecycle.load(std::memory_order_acquire);
        result.state = state & STATE_MASK;
        result.failures = (state >> FAILURE_SHIFT) & FAILURE_MASK;
        result.firstFailure = (state >> FIRST_SHIFT) & FAILURE_MASK;
        return true;
    }

    void publish(uint64_t cycle) {
        if (!owner() || !active()) return;
        if (cycle < m_lastCycle) { fail(Clock); return; }
        m_lastCycle = cycle;
        m_cycle.store(cycle, std::memory_order_release);
    }
    uint64_t event(uint64_t cycle, Kind kind, uint32_t address, uint32_t value, uint32_t width,
                   uint64_t parent = 0, uint32_t detail = 0, uint32_t flags = 0) {
        if (!owner() || !active()) return 0;
        publish(cycle);
        if (failures() || m_status.events == m_events.size()) {
            if (!failures()) fail(Events);
            if (m_status.droppedEvents != UINT32_MAX) ++m_status.droppedEvents;
            return 0;
        }
        Event result;
        result.cycle = cycle;
        result.frame = m_frame.load(std::memory_order_acquire);
        result.kind = uint32_t(kind); result.address = address;
        result.value = value; result.width = width; result.parent = parent;
        result.detail = detail; result.flags = flags;
        m_events[m_status.events++] = result;
        return m_status.events;
    }

    void pcm(const float* data, uint32_t frames, uint32_t voices, uint32_t disc, uint32_t flags,
             uint64_t voiceOffset) {
        if (!active()) return;
        if (!data || !frames || frames > MAX_CHUNK || voices > frames || disc > frames || (flags & ~ValidBlockFlags)) {
            fail(Bounds);
            if (!(flags & Submitted)) fail(Output);
            drop(frames); return;
        }
        auto rejected = [&] {
            if (!(flags & Submitted)) fail(Output);
            drop(frames);
        };
        if (failures()) { rejected(); return; }
        if (frames > m_status.frameLimit - m_status.frames) { fail(Frames); rejected(); return; }
        if (m_status.blocks == m_blocks.size()) { fail(Blocks); rejected(); return; }
        Block block;
        block.lastPublishedCpuCycle = m_cycle.load(std::memory_order_acquire);
        block.voiceOffset = voiceOffset;
        block.offset = m_status.frames; block.frames = frames;
        block.voices = voices; block.disc = disc; block.flags = flags;
        std::memcpy(m_pcm.data() + size_t(m_status.frames) * 2, data, size_t(frames) * 2 * sizeof(float));
        m_blocks[m_status.blocks++] = block;
        m_status.frames += frames;
        m_frame.store(m_status.frames, std::memory_order_release);
        if (!(flags & Submitted)) fail(Output);
    }
    uint64_t workerToken() const {
        const auto state = m_lifecycle.load(std::memory_order_acquire);
        return (state & STATE_MASK) == 1 ? state >> EPOCH_SHIFT : 0;
    }
    // The append lock orders observations. Only queue hooks also hold the queue's
    // existing mutex. No recorder lock spans a device operation or a wait.
    void schedule(uint64_t epoch, const Schedule& value) {
        if (!epoch) return;
        std::lock_guard lock(m_scheduleMutex);
        if (epoch != workerToken()) return;
        const auto kind = uint32_t(value.kind);
        if (kind < 1 || kind > 8 || value.source > 1) { fail(Bounds); return; }
        const unsigned actor = kind >= 6 ? 0 : kind <= 3 ? 1 : kind == 4 ? 2 : 2 + value.source;
        const auto thread = std::this_thread::get_id();
        if ((actor == 0) != (thread == m_owner) ||
            (m_scheduleThreads[actor] != std::thread::id{} && m_scheduleThreads[actor] != thread)) {
            fail(Thread); return;
        }
        m_scheduleThreads[actor] = thread;
        if (failures() || m_scheduleCount == m_scheduling.size()) {
            if (!failures()) fail(Scheduling);
            if (m_droppedScheduling != UINT32_MAX) ++m_droppedScheduling;
            return;
        }
        m_scheduling[m_scheduleCount++] = value;
    }
    void gain(uint64_t epoch, uint64_t voiceOffset, uint32_t sample, uint32_t voice, int32_t left, int32_t right) {
        if (!epoch) return;
        std::lock_guard lock(m_workerMutex);
        if (epoch != workerToken() || !workerThread()) return;
        if (voice >= VOICE_COUNT || sample >= MAX_CHUNK || voiceOffset > UINT64_MAX - sample) {
            fail(Bounds); return;
        }
        const auto offset = voiceOffset + sample;
        const auto bit = uint32_t(1) << voice;
        if ((m_gainSeen & bit) && offset <= m_gainLastOffset[voice]) {
            fail(Discontinuity); return;
        }
        m_gainLastOffset[voice] = offset;
        if ((m_gainSeen & bit) && m_gainLast[voice] == std::array<int32_t, 2>{left, right}) return;
        m_gainSeen |= bit;
        m_gainLast[voice] = {left, right};
        if (failures() || m_gainCount == m_gains.size()) {
            if (!failures()) fail(Gains);
            if (m_droppedGains != UINT32_MAX) ++m_droppedGains;
            return;
        }
        m_gains[m_gainCount++] = {offset, voice, left, right};
    }
    void envelopeInput(uint64_t epoch, uint64_t voiceOffset, uint32_t sample, uint32_t voice,
                       int32_t phase, int32_t rate, int32_t exponential, int32_t increasing, int32_t sustainLevel) {
        if (!epoch) return;
        std::lock_guard lock(m_workerMutex);
        if (epoch != workerToken() || !workerThread()) return;
        constexpr std::array<int32_t, 4> maxRate{127, 15, 127, 31};
        if (voice >= VOICE_COUNT || sample >= MAX_CHUNK || voiceOffset > UINT64_MAX - sample ||
            phase < 0 || phase > 3 || rate < 0 || rate > maxRate[phase] ||
            exponential < 0 || exponential > 1 || increasing < 0 || increasing > 1 ||
            (phase == 0 && increasing != 1) || (phase == 1 && (exponential != 1 || increasing != 0)) ||
            (phase == 3 && increasing != 0) ||
            (phase == 1 ? sustainLevel < 0 || sustainLevel > 15 : sustainLevel != -1)) {
            fail(Bounds); return;
        }
        const auto offset = voiceOffset + sample;
        const auto bit = uint32_t(1) << voice;
        if ((m_envelopeInputSeen & bit) && offset <= m_envelopeInputLastOffset[voice]) {
            fail(Discontinuity); return;
        }
        m_envelopeInputLastOffset[voice] = offset;
        const std::array<int32_t, 5> inputs{phase, rate, exponential, increasing, sustainLevel};
        if ((m_envelopeInputSeen & bit) && m_envelopeInputLast[voice] == inputs) return;
        m_envelopeInputSeen |= bit;
        m_envelopeInputLast[voice] = inputs;
        if (failures() || m_envelopeInputCount == m_envelopeInputs.size()) {
            if (!failures()) fail(EnvelopeInputs);
            if (m_droppedEnvelopeInputs != UINT32_MAX) ++m_droppedEnvelopeInputs;
            return;
        }
        m_envelopeInputs[m_envelopeInputCount++] = {offset, voice, phase, rate, exponential, increasing, sustainLevel};
    }
    void workerBatch(uint64_t epoch, uint64_t voiceOffset, uint32_t frames, uint32_t keyOnMask,
                     const std::array<int32_t, VOICE_COUNT>& preSampleSteps,
                     const std::array<int32_t, VOICE_COUNT>& stopSamples, int32_t firstReverbSample) {
        if (!epoch) return;
        std::lock_guard lock(m_workerMutex);
        if (epoch != workerToken() || !workerThread()) return;
        if (!frames || frames > MAX_CHUNK || keyOnMask > 0xffffff || voiceOffset > UINT64_MAX - frames ||
            firstReverbSample < -1 || firstReverbSample >= int32_t(frames)) {
            fail(Bounds); return;
        }
        for (const auto sample : stopSamples) {
            if (sample < -1 || sample >= int32_t(frames)) { fail(Bounds); return; }
        }
        if (failures() || m_workerCount == m_workers.size()) {
            if (!failures()) fail(Workers);
            if (m_droppedWorkers != UINT32_MAX) ++m_droppedWorkers;
            return;
        }
        if (m_workerCount && voiceOffset != m_workers[m_workerCount - 1].voiceOffset +
                                              m_workers[m_workerCount - 1].frames) {
            fail(Discontinuity); return;
        }
        m_workers[m_workerCount++] = {voiceOffset, 0, frames, keyOnMask, 0, preSampleSteps, stopSamples, firstReverbSample};
    }
    void workerQueued(uint64_t epoch, uint64_t offset, uint32_t frames) {
        if (!epoch) return;
        std::lock_guard lock(m_workerMutex);
        if (epoch != workerToken() || !workerThread()) return;
        if (!frames || frames > MAX_CHUNK || offset > UINT64_MAX - frames) { fail(Bounds); return; }
        while (m_workerQueued < m_workerCount) {
            auto& batch = m_workers[m_workerQueued];
            if (batch.voiceOffset + batch.frames > offset + frames) break;
            if (batch.voiceOffset >= offset) {
                batch.enqueueOffset = offset;
                batch.enqueueFrames = frames;
            }
            ++m_workerQueued;
        }
    }
    void streamingLoss(uint32_t frames) {
        if (!owner() || !active()) return;
        m_status.streamingDropped += std::min<uint64_t>(frames, UINT64_MAX - m_status.streamingDropped);
        fail(StreamingLoss);
    }
    void fail(Failure reason) {
        auto state = m_lifecycle.load(std::memory_order_acquire);
        const auto epoch = state >> EPOCH_SHIFT;
        while ((state & STATE_MASK) == 1 && (state >> EPOCH_SHIFT) == epoch) {
            auto update = state | (uint64_t(reason) << FAILURE_SHIFT);
            if (!((state >> FIRST_SHIFT) & FAILURE_MASK)) update |= uint64_t(reason) << FIRST_SHIFT;
            if (m_lifecycle.compare_exchange_weak(state, update, std::memory_order_acq_rel)) return;
        }
    }

    bool blocks(uint32_t first, uint32_t count, Block* out) {
        if (!frozen() || count > MAX_BATCH || first > m_status.blocks || count > m_status.blocks - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_blocks.data() + first, size_t(count) * sizeof(Block));
        return true;
    }
    bool events(uint32_t first, uint32_t count, Event* out) {
        if (!frozen() || count > MAX_BATCH || first > m_status.events || count > m_status.events - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_events.data() + first, size_t(count) * sizeof(Event));
        return true;
    }
    bool workers(uint32_t first, uint32_t count, Worker* out) {
        std::lock_guard lock(m_workerMutex);
        if (!frozen() || count > MAX_BATCH || first > m_workerCount || count > m_workerCount - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_workers.data() + first, size_t(count) * sizeof(Worker));
        return true;
    }
    bool gains(uint32_t first, uint32_t count, Gain* out) {
        std::lock_guard lock(m_workerMutex);
        if (!frozen() || count > MAX_BATCH || first > m_gainCount || count > m_gainCount - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_gains.data() + first, size_t(count) * sizeof(Gain));
        return true;
    }
    bool envelopeInputs(uint32_t first, uint32_t count, EnvelopeInput* out) {
        std::lock_guard lock(m_workerMutex);
        if (!frozen() || count > MAX_BATCH || first > m_envelopeInputCount || count > m_envelopeInputCount - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_envelopeInputs.data() + first, size_t(count) * sizeof(EnvelopeInput));
        return true;
    }
    bool scheduling(uint32_t first, uint32_t count, Schedule* out) {
        std::lock_guard lock(m_scheduleMutex);
        if (!frozen() || count > MAX_BATCH || first > m_scheduleCount || count > m_scheduleCount - first ||
            (count && !out)) return false;
        if (count) std::memcpy(out, m_scheduling.data() + first, size_t(count) * sizeof(Schedule));
        return true;
    }
    bool snapshot(Snapshot& out) {
        if (!frozen() || m_status.events > MAX_RECORDS) return false;
        if (!status(out.status)) return false;
        out.epoch = identity()[1];
        out.events.assign(m_events.begin(), m_events.begin() + m_status.events);
        return true;
    }
    bool sampleRange(uint32_t offset, uint32_t frames) {
        return frozen() && offset <= m_status.frames && frames <= m_status.frames - offset;
    }
    bool samples(uint32_t offset, uint32_t frames, float* out) {
        if (!sampleRange(offset, frames) || (!out && frames)) return false;
        if (frames) std::memcpy(out, m_pcm.data() + size_t(offset) * 2, size_t(frames) * 2 * sizeof(float));
        return true;
    }

  private:
    bool workerThread() {
        const auto thread = std::this_thread::get_id();
        if (m_workerThread == std::thread::id{}) m_workerThread = thread;
        if (m_workerThread == thread && thread != m_owner) return true;
        fail(Thread); return false;
    }
    bool owner() {
        if (std::this_thread::get_id() == m_owner) return true;
        fail(Thread); return false;
    }
    bool frozen() { return owner() && (m_lifecycle.load(std::memory_order_acquire) & STATE_MASK) == 2; }
    void drop(uint32_t frames) {
        m_status.droppedFrames += std::min<uint64_t>(frames, UINT64_MAX - m_status.droppedFrames);
    }
    static constexpr uint64_t STATE_MASK = 3, FAILURE_MASK = 8191;
    static constexpr unsigned FAILURE_SHIFT = 2, FIRST_SHIFT = 15, EPOCH_SHIFT = 28;
    static constexpr uint64_t MAX_EPOCH = UINT64_MAX >> EPOCH_SHIFT;
    uint32_t failures() const { return (m_lifecycle.load(std::memory_order_acquire) >> FAILURE_SHIFT) & FAILURE_MASK; }
    const std::thread::id m_owner;
    uint64_t m_epoch = 0;
    std::atomic<uint64_t> m_lifecycle{0};
    std::atomic<uint64_t> m_cycle{0}, m_frame{0};
    uint64_t m_lastCycle = 0;
    Status m_status;
    std::vector<float> m_pcm;
    std::vector<Block> m_blocks;
    std::vector<Event> m_events;
    std::mutex m_workerMutex;
    std::thread::id m_workerThread;
    uint32_t m_workerCount = 0, m_workerQueued = 0, m_droppedWorkers = 0;
    std::vector<Worker> m_workers;
    uint32_t m_gainCount = 0, m_droppedGains = 0, m_gainSeen = 0;
    std::array<uint64_t, VOICE_COUNT> m_gainLastOffset{};
    std::array<std::array<int32_t, 2>, VOICE_COUNT> m_gainLast{};
    std::vector<Gain> m_gains;
    uint32_t m_envelopeInputCount = 0, m_droppedEnvelopeInputs = 0, m_envelopeInputSeen = 0;
    std::array<uint64_t, VOICE_COUNT> m_envelopeInputLastOffset{};
    std::array<std::array<int32_t, 5>, VOICE_COUNT> m_envelopeInputLast{};
    std::vector<EnvelopeInput> m_envelopeInputs;
    std::mutex m_scheduleMutex;
    std::array<std::thread::id, 4> m_scheduleThreads{};
    uint32_t m_scheduleCount = 0, m_droppedScheduling = 0;
    std::vector<Schedule> m_scheduling;
};

}
