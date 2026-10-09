// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace PCSX::History::CD {

class Origins {
  public:
    static constexpr uint32_t Size = 2352;
    void reset() { m_origins.fill(0); m_latest = 0; }
    bool assign(uint32_t offset, uint32_t length, uint64_t mutation) {
        if (!length || offset >= Size || length > Size - offset || mutation <= m_latest) return false;
        for (uint32_t n = offset; n < offset + length; ++n) m_origins[n] = mutation;
        m_latest = mutation;
        return true;
    }
    bool get(uint32_t offset, uint64_t& mutation) const {
        if (offset >= Size) return false;
        mutation = m_origins[offset];
        return true;
    }
  private:
    std::array<uint64_t, Size> m_origins{};
    uint64_t m_latest = 0;
};

class Response {
  public:
    struct Draft {
        uint64_t id = 0, operation = 0, displaced = 0;
        uint32_t revision = 0, size = 0;
    };
    void reset() { m_serial = m_published = 0; m_draft = {}; }
    bool resize(uint64_t operation, uint32_t size, Draft& result) {
        if (!operation || size > 16 || (m_draft.id && m_draft.operation != operation)) return false;
        if (m_draft.id) {
            if (m_draft.revision == UINT32_MAX) return false;
            ++m_draft.revision;
            m_draft.displaced = 0;
        } else {
            if (m_serial == UINT64_MAX) return false;
            m_draft = {++m_serial, operation, m_published, 1, 0};
            m_published = 0;
        }
        m_draft.size = size;
        result = m_draft;
        return true;
    }
    bool seal(uint64_t operation, bool publish, Draft& result) {
        if (!m_draft.id || operation != m_draft.operation) return false;
        result = m_draft;
        m_published = publish ? m_draft.id : 0;
        m_draft = {};
        return true;
    }
    uint64_t published() const { return m_published; }
    uint64_t current() const { return m_draft.id ? m_draft.id : m_published; }
    bool building() const { return m_draft.id != 0; }
  private:
    uint64_t m_serial = 0, m_published = 0;
    Draft m_draft{};
};

}
