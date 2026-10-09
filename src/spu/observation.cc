// SPDX-License-Identifier: GPL-2.0-or-later
#include "spu/interface.h"
#include "core/transactions.h"

void PCSX::SPU::impl::invalidateCapture() {
    m_audioOut.capture().fail(Capture::Discontinuity);
    History::CD::invalidate();
}

void PCSX::SPU::impl::observeCDFeed(uint32_t reason, const xa_decode_t* xap,
                                 uint32_t frames, bool cdda, uint64_t event) {
    if (!m_audioOut.capture().ownerThread() || !History::ownerThread()) {
        m_audioOut.capture().fail(Capture::Thread);
        History::fail(History::Failure::Thread); return;
    }
    if (!History::CD::active()) return;
    if (!History::CD::feedActive()) { History::fail(History::Failure::Association); return; }
    const auto identity = m_audioOut.capture().identity();
    History::CD::feedOutcome(reason, xap ? xap->freq : 0, xap ? xap->nsamples : 0,
        xap ? xap->stereo : 0, frames, cdda, identity[0], identity[1], event);
}

uint64_t PCSX::SPU::impl::captureAccess(uint32_t address, uint32_t value, uint32_t width, bool read) {
    if (!m_audioOut.capture().ownerThread()) { m_audioOut.capture().fail(Capture::Thread); return 0; }
    const auto parent = m_captureParent;
    const uint32_t physical = address & 0x1fffffff;
    if (physical >= 0x1f801c00 && physical < 0x1f801e00) {
        m_audioOut.checkCaptureSettings();
        const auto next = m_audioOut.capture().event(g_emulator->m_cpu->m_regs.cycle,
            read ? Capture::Kind::Read : Capture::Kind::Access, address, value, width, parent);
        m_captureParent = next;
    }
    return parent;
}

void PCSX::SPU::impl::captureReturn(uint32_t address, uint32_t value, uint32_t width) {
    if (!m_audioOut.capture().ownerThread()) { m_audioOut.capture().fail(Capture::Thread); return; }
    const uint32_t physical = address & 0x1fffffff;
    if (physical >= 0x1f801c00 && physical < 0x1f801e00) {
        m_audioOut.capture().event(g_emulator->m_cpu->m_regs.cycle, Capture::Kind::Return,
                                   address, value, width, m_captureParent);
    }
}

std::array<uint32_t, 2> PCSX::SPU::impl::captureInfluences() const {
    std::array<uint32_t, 2> result{};
    for (unsigned n = 0; n < MAXCHAN; ++n) {
        if (s_chan[n].data.get<Chan::Mute>().value) result[0] |= 1u << n;
        if (s_chan[n].data.get<Chan::Solo>().value) result[1] |= 1u << n;
    }
    return result;
}
