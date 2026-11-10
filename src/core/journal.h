// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace PCSX::History {

inline constexpr uint32_t RECORD_BATCH_LIMIT = 65536;
inline constexpr uint32_t CD_EVENT_LIMIT = 67108864;
inline constexpr uint32_t PAYLOAD_BATCH_LIMIT = 8388608;
inline constexpr uint32_t CD_PAYLOAD_LIMIT = 2147483648u;

enum class Failure : uint32_t {
    Events = 1, Bytes = 2, Thread = 4, Unsupported = 8,
    Association = 16, Discontinuity = 32, Bounds = 64, Clock = 128,
};

struct Record {
    uint64_t sequence = 0, cycle = 0, request = 0, related = 0;
    uint32_t kind = 0, device = 0;
    std::array<uint32_t, 8> data{};
    uint32_t offset = 0, length = 0;
};
static_assert(sizeof(Record) == 80);

struct Status {
    uint64_t beginCycle = 0, endCycle = 0;
    uint32_t version = 1, state = 0, events = 0, bytes = 0;
    uint32_t failures = 0, firstFailure = 0, dropped = 0;
};
static_assert(sizeof(Status) == 48);

struct Storage {
    std::vector<Record> records;
    std::vector<uint8_t> payload;
};
struct Snapshot {
    Status status;
    uint64_t epoch = 0;
    std::shared_ptr<const Storage> storage;
};

class Journal {
  public:
    explicit Journal(std::thread::id owner = std::this_thread::get_id()) : m_owner(owner) {}
    bool begin(uint32_t events, uint32_t bytes, uint64_t cycle, bool cd = false) {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state != 0 || events == 0 || events > (cd ? CD_EVENT_LIMIT : RECORD_BATCH_LIMIT) ||
            bytes > (cd ? CD_PAYLOAD_LIMIT : PAYLOAD_BATCH_LIMIT)) return false;
        try {
            auto storage = std::make_shared<Storage>();
            storage->records.reserve(events);
            storage->payload.reserve(bytes);
            m_storage = std::move(storage);
        } catch (...) {
            return false;
        }
        m_eventLimit = events;
        m_byteLimit = bytes;
        m_status = {};
        m_status.state = 1;
        m_status.beginCycle = cycle;
        m_status.endCycle = cycle;
        return true;
    }

    template <typename Clock>
    bool append(Record record, Clock clock, const void* bytes = nullptr, uint32_t length = 0) {
        std::lock_guard guard(m_mutex);
        if (m_status.state != 1) return false;
        if (!owner()) return drop(Failure::Thread);
        if (m_status.failures) return drop();
        if (m_storage->records.size() >= m_eventLimit) return drop(Failure::Events);
        if (length > m_byteLimit - m_storage->payload.size()) return drop(Failure::Bytes);
        if (length && !bytes) return drop(Failure::Bounds);
        uint64_t cycle = clock();
        if (cycle < m_status.endCycle) return drop(Failure::Clock);
        record.sequence = m_storage->records.size() + 1;
        record.cycle = cycle;
        record.offset = m_storage->payload.size();
        record.length = length;
        if (length) {
            const auto* first = static_cast<const uint8_t*>(bytes);
            m_storage->payload.insert(m_storage->payload.end(), first, first + length);
        }
        m_storage->records.push_back(record);
        m_status.endCycle = cycle;
        m_status.events = m_storage->records.size();
        m_status.bytes = m_storage->payload.size();
        return true;
    }

    void fail(Failure failure) {
        std::lock_guard guard(m_mutex);
        if (m_status.state == 1) {
            mark(owner() ? failure : Failure::Thread);
        }
    }
    bool stop(uint64_t cycle) {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state != 1) return false;
        if (cycle < m_status.endCycle) mark(Failure::Clock);
        else m_status.endCycle = cycle;
        m_status.state = 2;
        return true;
    }
    bool clear() {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state == 1) return false;
        m_storage.reset();
        m_status = {};
        return true;
    }
    bool status(Status& out) const {
        std::lock_guard guard(m_mutex);
        if (!owner()) return false;
        out = m_status;
        return true;
    }
    bool records(uint32_t first, uint32_t count, Record* out) const {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state != 2 || count > RECORD_BATCH_LIMIT || first > m_storage->records.size() ||
            count > m_storage->records.size() - first || (count && !out)) return false;
        if (count) std::memcpy(out, m_storage->records.data() + first, size_t(count) * sizeof(Record));
        return true;
    }
    bool bytes(uint32_t offset, uint32_t length, void* out) const {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state != 2 || offset > m_storage->payload.size() ||
            length > m_storage->payload.size() - offset || (length && !out)) return false;
        if (length) std::memcpy(out, m_storage->payload.data() + offset, length);
        return true;
    }
    bool snapshot(Snapshot& out) const {
        std::lock_guard guard(m_mutex);
        if (!owner() || m_status.state != 2 || m_storage->records.size() > m_eventLimit || m_storage->payload.size() > m_byteLimit)
            return false;
        out.status = m_status;
        out.storage = m_storage;
        return true;
    }
    bool owner() const { return std::this_thread::get_id() == m_owner; }

  private:
    void mark(Failure failure) {
        uint32_t bit = static_cast<uint32_t>(failure);
        if (!m_status.firstFailure) m_status.firstFailure = bit;
        m_status.failures |= bit;
    }
    bool drop(Failure failure) { mark(failure); return drop(); }
    bool drop() {
        if (m_status.dropped != std::numeric_limits<uint32_t>::max()) ++m_status.dropped;
        return false;
    }
    const std::thread::id m_owner;
    mutable std::mutex m_mutex;
    std::shared_ptr<Storage> m_storage;
    Status m_status;
    uint32_t m_eventLimit = 0, m_byteLimit = 0;
};

}
