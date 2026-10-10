// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace PCSX::SPU {

class Capture {
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
  public:
    enum Failure : uint32_t {
        Frames = 1, Blocks = 2, Events = 4, Thread = 8, Bounds = 16,
        Clock = 32, Discontinuity = 64, StreamingLoss = 128, Output = 256,
    };
    enum class Kind : uint32_t { Access = 1, Write = 2, Feed = 3, Read = 4, Result = 5, Return = 6 };
    enum BlockFlag : uint32_t { Mono = 1, Muted = 2, Submitted = 4, ValidBlockFlags = 7 };
    struct Block {
        uint64_t lastPublishedCpuCycle = 0;
        uint32_t offset = 0, frames = 0, voices = 0, disc = 0, flags = 0;
    };
    struct Event {
        uint64_t cycle = 0, frame = 0, parent = 0;
        uint32_t kind = 0, address = 0, value = 0, width = 0, detail = 0, flags = 0;
    };
    struct Status {
        uint64_t beginCycle = 0, endCycle = 0, droppedFrames = 0, streamingDropped = 0;
        uint32_t version = 2, state = 0, frames = 0, blocks = 0, events = 0;
        uint32_t frameLimit = 0, blockLimit = 0, eventLimit = 0;
        uint32_t failures = 0, firstFailure = 0, droppedEvents = 0;
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
        try {
            std::vector<float> pcm(size_t(frames) * 2);
            std::vector<Block> records(blocks);
            std::vector<Event> writes(events);
            m_pcm.swap(pcm); m_blocks.swap(records); m_events.swap(writes);
        } catch (...) { return false; }
        m_status = {};
        m_status.beginCycle = m_status.endCycle = cycle;
        m_status.frameLimit = frames; m_status.blockLimit = blocks; m_status.eventLimit = events;
        m_cycle.store(cycle); m_frame.store(0);
        m_lastCycle = cycle;
        m_lifecycle.store((++m_epoch << EPOCH_SHIFT) | 1, std::memory_order_release);
        return true;
    }

    bool stop(uint64_t cycle) {
        if (!owner() || !active()) return false;
        publish(cycle);
        m_status.endCycle = cycle;
        auto state = m_lifecycle.load();
        while (!m_lifecycle.compare_exchange_weak(state, (state & ~STATE_MASK) | 2)) {}
        return true;
    }
    bool clear() {
        if (!owner() || active()) return false;
        std::vector<float>().swap(m_pcm);
        std::vector<Block>().swap(m_blocks);
        std::vector<Event>().swap(m_events);
        m_status = {};
        m_cycle.store(0); m_frame.store(0);
        m_lifecycle.store(m_epoch << EPOCH_SHIFT, std::memory_order_release);
        return true;
    }
    bool status(Status& result) {
        if (!owner()) return false;
        result = m_status;
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

    void pcm(const float* data, uint32_t frames, uint32_t voices, uint32_t disc, uint32_t flags) {
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
        block.offset = m_status.frames; block.frames = frames;
        block.voices = voices; block.disc = disc; block.flags = flags;
        std::memcpy(m_pcm.data() + size_t(m_status.frames) * 2, data, size_t(frames) * 2 * sizeof(float));
        m_blocks[m_status.blocks++] = block;
        m_status.frames += frames;
        m_frame.store(m_status.frames, std::memory_order_release);
        if (!(flags & Submitted)) fail(Output);
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
    bool owner() {
        if (std::this_thread::get_id() == m_owner) return true;
        fail(Thread); return false;
    }
    bool frozen() { return owner() && (m_lifecycle.load(std::memory_order_acquire) & STATE_MASK) == 2; }
    void drop(uint32_t frames) {
        m_status.droppedFrames += std::min<uint64_t>(frames, UINT64_MAX - m_status.droppedFrames);
    }
    static constexpr uint64_t STATE_MASK = 3, FAILURE_MASK = 511;
    static constexpr unsigned FAILURE_SHIFT = 2, FIRST_SHIFT = 11, EPOCH_SHIFT = 20;
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
};

}
