/***************************************************************************
 *   Copyright (C) 2022 PCSX-Redux authors                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

/*
 * Handles all CD-ROM registers and functions.
 */

#include "core/cdrom.h"

#include <magic_enum/magic_enum_all.hpp>
#include <string_view>
#include <bit>
#include "core/transactions.h"

#include "cdrom/iso9660-reader.h"
#include "core/cdromlogger.h"
#include "core/debug.h"
#include "core/psxdma.h"
#include "core/psxemulator.h"
#include "spu/interface.h"
#include "support/strings-helpers.h"
#include "supportpsx/iec-60908b.h"

namespace {

using namespace std::literals;

// The buffer/decoder chip the PSX CPU will talk to is the CXD1199, which
// datasheet can be found at https://archive.org/details/cxd-1199

class CDRomImpl final : public PCSX::CDRom {
    enum Commands {
        CdlSync = 0,
        CdlNop = 1,
        CdlSetLoc = 2,
        CdlPlay = 3,
        CdlForward = 4,
        CdlBackward = 5,
        CdlReadN = 6,
        CdlStandby = 7,
        CdlStop = 8,
        CdlPause = 9,
        CdlInit = 10,
        CdlMute = 11,
        CdlDemute = 12,
        CdlSetFilter = 13,
        CdlSetMode = 14,
        CdlGetParam = 15,
        CdlGetLocL = 16,
        CdlGetLocP = 17,
        CdlReadT = 18,
        CdlGetTN = 19,
        CdlGetTD = 20,
        CdlSeekL = 21,
        CdlSeekP = 22,
        CdlSetClock = 23,
        CdlGetClock = 24,
        CdlTest = 25,
        CdlID = 26,
        CdlReadS = 27,
        CdlReset = 28,
        CdlGetQ = 29,
        CdlReadToc = 30,
    };

    std::string_view commandName(uint8_t command) {
        if (command >= c_cdCmdEnumCount) {
            return "Unknown";
        } else {
            return magic_enum::enum_names<Commands>()[command];
        }
    }

    static constexpr size_t c_cdCmdEnumCount = magic_enum::enum_count<Commands>();
    static constexpr bool isValidBCD(uint8_t value) { return (value & 0x0f) <= 9 && (value & 0xf0) <= 0x90; }
    static std::optional<PCSX::IEC60908b::MSF> getMSF(const uint8_t *msf) {
        bool validBCD = isValidBCD(msf[0]) && isValidBCD(msf[1]) && isValidBCD(msf[2]);
        if (!validBCD) return {};
        uint8_t m = PCSX::IEC60908b::btoi(msf[0]);
        uint8_t s = PCSX::IEC60908b::btoi(msf[1]);
        uint8_t f = PCSX::IEC60908b::btoi(msf[2]);

        if (s >= 60) return {};
        if (f >= 75) return {};
        return PCSX::IEC60908b::MSF(m, s, f);
    }

    uint32_t rand() {
        m_seed *= 14726776315600504853ull;
        return m_seed >> 9;
    }

    bool acceptsPreparedState(const PCSX::SaveStates::CDRom& cd, uint32_t mask) const noexcept override {
        return acceptsPreparedMedia(cd, mask, m_iso);
    }

    bool acceptsPreparedMedia(const PCSX::SaveStates::CDRom& cd, uint32_t mask,
                              const std::shared_ptr<PCSX::CDRIso>& iso) const noexcept override {
        using namespace PCSX::SaveStates;
        using Access = PCSX::CDRIso::StateAccess;
        const auto msf = [](const auto& field) {
            return MSF{field.template get<CDMSFMinute>().decodedValue(),
                       field.template get<CDMSFSecond>().decodedValue(),
                       field.template get<CDMSFFrame>().decodedValue()};
        };
        const auto position = msf(cd.get<CDCurrentPosition>());
        const auto seek = msf(cd.get<CDSeekPosition>());
        const auto access = [&iso](Access kind, MSF at, uint8_t track = 0) {
            return iso && iso->acceptsStateAccess(kind, at, track);
        };
        const auto data = [&](MSF at, bool resetsXA) {
            if (!access(Access::DataSector, at)) return false;
            const auto track = iso->getTrack(at);
            if (track == 0 || iso->getTrackType(track) == PCSX::CDRIso::TrackType::CDDA || resetsXA ||
                !cd.get<CDRealtime>().decodedValue() || cd.get<CDXAEnded>().decodedValue() ||
                cd.get<CDXAFirstSector>().decodedValue()) return true;
            if (!m_xaFormatReady) return false;
            return (m_xa.freq == 18900 || m_xa.freq == 37800) && (m_xa.nbits == 4 || m_xa.nbits == 8) &&
                   (m_xa.stereo == 0 || m_xa.stereo == 1) && m_xa.nsamples >= 0 &&
                   m_xa.nsamples <= (m_xa.stereo ? 2016 : 4032);
        };
        const auto audio = [&](MSF at) {
            if (!access(Access::TrackAddress, at)) return false;
            if (at >= iso->getTD(0)) return true;
            if (!access(Access::Position, at)) return false;
            uint8_t locP[8];
            if (iso->getLocP(at, locP) && cd.get<CDAutoPause>().decodedValue() &&
                cd.get<CDPlayTrack>().decodedValue() != 0 && locP[0] != cd.get<CDPlayTrack>().decodedValue())
                return true;
            return access(Access::AudioSector, at);
        };
        const auto command = [&](const auto& queue, bool start) {
            const auto op = queue.template get<CDQueueValue>().decodedValue();
            const auto payload = queue.template get<CDQueuePayload>().decodedData();
            const auto size = queue.template get<CDQueuePayloadSize>().decodedValue();
            switch (op) {
                case CdlPlay: {
                    bool seeks = cd.get<CDSetLocPending>().decodedValue();
                    MSF next = seeks ? seek : position;
                    if (!access(Access::TrackCount, position)) return false;
                    if (size > 0) {
                        unsigned track = isValidBCD(payload[0]) ? PCSX::IEC60908b::btoi(payload[0]) : 100;
                        if (track != 0) {
                            if (track > iso->getTN()) {
                                if (!access(Access::Position, position)) return false;
                                track = iso->getTrack(position);
                            }
                            if (track != 0) {
                                if (!access(Access::TrackAddress, position, track)) return false;
                                next = iso->getTD(track);
                                seeks = true;
                            }
                        }
                    }
                    return (!seeks || access(Access::Position, next)) && audio(next);
                }
                case CdlReadN: case CdlReadS:
                    return access(Access::Position, seek) && data(seek, true);
                case CdlGetLocP: return access(Access::Position, position);
                case CdlGetTN: return access(Access::TrackCount, position);
                case CdlGetTD:
                    if (!isValidBCD(payload[0])) return true;
                    return access(Access::TrackCount, position) &&
                           access(Access::TrackAddress, position, PCSX::IEC60908b::btoi(payload[0]));
                case CdlSeekL: case CdlSeekP: return access(Access::Position, seek);
                default: return true;
            }
        };
        if (mask & (1u << static_cast<unsigned>(PCSX::Schedule::CDRCOMMANDS))) {
            const auto& queue = cd.get<CDCommandExecuting>();
            const auto op = queue.get<CDQueueValue>().decodedValue();
            if (op >= c_cdCmdEnumCount || !c_commandsHandlers[op] || !command(queue, false)) return false;
        }
        if (mask & (1u << static_cast<unsigned>(PCSX::Schedule::CDRFIFO))) {
            const auto valueEmpty = [](const auto& q) {
                return !q.template get<CDQueueHasValue>().decodedValue() ||
                       q.template get<CDQueueValueRead>().decodedValue();
            };
            const auto empty = [&](const auto& q) {
                return valueEmpty(q) && (q.template get<CDQueuePayloadSize>().decodedValue() == 0 ||
                                        q.template get<CDQueueHitMax>().decodedValue());
            };
            const auto& head = cd.get<CDResponseFifo0>();
            const auto& tail = cd.get<CDResponseFifo1>();
            const auto& queue = cd.get<CDCommandFifo>();
            const bool promote = valueEmpty(head) && !empty(tail);
            const bool ready = promote ? valueEmpty(tail) : valueEmpty(head) && empty(tail);
            const auto op = queue.get<CDQueueValue>().decodedValue();
            if (ready && !empty(queue) && op < c_cdCmdEnumCount && c_commandsHandlers[op] &&
                (c_commandsArgumentsCount[op] < 0 ||
                 c_commandsArgumentsCount[op] == queue.get<CDQueuePayloadSize>().decodedValue()) &&
                !command(queue, true)) return false;
        }
        if (!(mask & (1u << static_cast<unsigned>(PCSX::Schedule::CDREAD)))) return true;
        const auto reading = cd.get<CDReadingState>().decodedValue();
        const auto type = cd.get<CDReadingType>().decodedValue();
        const auto status = cd.get<CDStatus>().decodedValue();
        if (reading == static_cast<uint8_t>(ReadingState::Seeking)) {
            if (!access(Access::Position, seek)) return false;
            return type == static_cast<uint8_t>(ReadingType::Play) ? audio(seek) : data(seek, false);
        }
        if (reading == static_cast<uint8_t>(ReadingState::Reading))
            return type == static_cast<uint8_t>(ReadingType::Play) ? audio(position) : data(position, false);
        if (status == static_cast<uint8_t>(Status::ReadingData)) return data(position, false);
        if (status == static_cast<uint8_t>(Status::PlayingCDDA)) return audio(position);
        return true;
    }

    void reset() override {
        m_seed = 9223521712174600777ull;
        m_dataFIFOIndex = 0;
        m_dataFIFOSize = 0;
        m_registerAddress = 0;
        m_currentPosition.reset();
        m_seekPosition.reset();
        m_speed = Speed::Simple;
        m_speedChanged = false;
        m_status = Status::Idle;
        m_dataRequested = false;
        m_interruptCauseMask = 0x1f;
        m_subheaderFilter = false;
        m_realtime = false;
        m_autoPause = false;
        m_report = false;
        m_setLocPending = false;
        m_muted = false;
        m_adpcmMuted = false;
        m_mode = 0;
        m_readSpan = ReadSpan::S2048;
        m_filterFile = 0;
        m_filterChannel = 0;
        m_xaEnded = false;
        m_xaFirstSector = true;
        xa_decode_reset(&m_xa);
        m_xaFormatReady = false;
        m_atv[0] = m_atvPending[0] = 0x80;
        m_atv[1] = m_atvPending[1] = 0;
        m_atv[2] = m_atvPending[2] = 0x80;
        m_atv[3] = m_atvPending[3] = 0;
        m_commandFifo.clear();
        m_commandExecuting.clear();
        m_responseFifo[0].clear();
        m_responseFifo[1].clear();
        m_readingState = ReadingState::None;
        m_historyNativeInitialized = true;
        PCSX::History::CD::invalidate();
    }

    void fifoScheduledCallback() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Fifo, 0, 0, 0);
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
        if (m_responseFifo[0].valueEmpty() && !m_responseFifo[1].empty()) {
            m_responseFifo[0] = m_responseFifo[1];
            m_historyQueueIds[2] = m_historyQueueIds[3];
            m_responseFifo[1].clear();
            raiseIRQIfUnmasked();
            if (debug) {
                PCSX::g_system->log(PCSX::LogClass::CDROM,
                                    "CD-Rom: response fifo sliding one response, triggering IRQ.\n");
            }
        }
        maybeStartCommand();
    }

    void commandsScheduledCallback() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Continuation, 0, 0, 0);
        auto command = m_commandExecuting.value;
        auto handler = c_commandsHandlers[command];
        invokeCommand(handler, m_commandExecuting, false);
    }

    void readScheduledCallback() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Read, 0, 0, 0);
        static const std::chrono::nanoseconds c_retryDelay = 50us;
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
        if (m_readingState == ReadingState::Seeking) {
            auto seekType = m_readingType == ReadingType::Play ? SeekType::CDDA : SeekType::DATA;
            auto seekDelay = computeSeekDelay(m_currentPosition, m_seekPosition, seekType, true);
            m_status = Status::Seeking;
            if (m_speedChanged) {
                m_speedChanged = false;
                seekDelay += 650ms;
            }
            m_currentPosition = m_seekPosition;
            scheduleRead(seekDelay + computeReadDelay());
            m_readingState = ReadingState::Reading;
            return;
        } else if (m_readingState == ReadingState::Reading) {
            m_readingState = ReadingState::None;
            if (m_readingType == ReadingType::Play) {
                startPlaying();
            } else {
                m_status = Status::ReadingData;
            }
        } else if ((m_status == Status::Idle) || (m_status == Status::Seeking)) {
            m_readingType = ReadingType::None;
            if (debug) {
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: readInterrupt: cancelling read.\n");
            }
            return;
        }

        switch (m_status) {
            case Status::ReadingData: {
                unsigned track = m_iso->getTrack(m_currentPosition);
                if (m_iso->getTrackType(track) == PCSX::CDRIso::TrackType::CDDA) {
                    m_status = Status::Idle;
                    maybeEnqueueError(4, 4);
                } else if (track == 0) {
                    m_status = Status::Idle;
                    maybeEnqueueError(4, 0x10);
                } else {
                    m_invalidLocL = false;
                    PCSX::History::CD::Media historyMedium(1, uint32_t(m_currentPosition.m) | (uint32_t(m_currentPosition.s)<<8) | (uint32_t(m_currentPosition.f)<<16), m_currentPosition.toLBA(), PCSX::History::CD::backing());
                    const bool readResult = m_iso->readTrack(m_currentPosition);
                    historyMedium.result(readResult, 0);
                    PCSX::g_emulator->m_cdromLogger->recordAccess(m_currentPosition.toLBA(),
                                                                  PCSX::CDRomLogger::AccessType::Data,
                                                                  PCSX::g_emulator->m_cpu->m_regs.cycle);
                    auto buffer = m_iso->getBuffer();
                    if (PCSX::History::CD::active()) {
                        if (!m_iso->historyBufferDefined()) PCSX::History::fail(PCSX::History::Failure::Bounds);
                        else PCSX::History::CD::dataMutation(2, 0, 2340, buffer, historyMedium.id());
                    }
                    memcpy(m_lastLocL, buffer, sizeof(m_lastLocL));
                    PCSX::History::CD::dataMutation(3, 0, sizeof(m_lastLocL), m_lastLocL, historyMedium.id());
                    uint32_t size = 0;
                    bool passToData = true;
                    if ((buffer[3] == 2) && (m_realtime || m_subheaderFilter)) {
                        PCSX::IEC60908b::SubHeaders subHeaders;
                        subHeaders.fromBuffer(buffer + 4);
                        // With RT or SF set, real-time audio sectors never reach the host as data, whatever the
                        // filter says; Form 2 data and audio without the RT submode bit still do (SCPH-9002).
                        if (subHeaders.isAudio() && subHeaders.isRealTime()) {
                            passToData = false;
                            if (m_realtime) playXASector(buffer + 4, subHeaders);
                        }
                    }
                    if (passToData) {
                        switch (m_readSpan) {
                            case ReadSpan::S2048:
                                size = 2048;
                                if (buffer[3] == 1) {
                                    memcpy(m_dataFIFO, buffer + 4, 2048);
                                } else {
                                    memcpy(m_dataFIFO, buffer + 12, 2048);
                                }
                                break;
                            case ReadSpan::S2328:
                                size = 2328;
                                memcpy(m_dataFIFO, buffer + 12, 2328);
                                break;
                            case ReadSpan::S2340:
                                size = 2340;
                                memcpy(m_dataFIFO, buffer, 2340);
                                break;
                        }
                    }
                    auto readDelay = computeReadDelay();
                    // A sector withheld from the host leaves the previous data sector in the buffer.
                    if (passToData) {
                        PCSX::History::CD::dataMutation(1, 0, size, m_dataFIFO, historyMedium.id());
                        m_dataFIFOIndex = 0;
                        m_dataFIFOPending = size;
                        m_dataFIFOSize = m_dataRequested ? size : 0;
                    }
                    m_currentPosition++;
                    if (debug) {
                        std::string msfFormat = fmt::format("{}", m_currentPosition);
                        PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: readInterrupt: advancing to %s.\n",
                                            msfFormat);
                    }
                    if (passToData) {
                        QueueElement ready;
                        ready.pushPayloadData(getStatus());
                        maybeTriggerIRQ(Cause::DataReady, ready);
                    }
                    scheduleRead(readDelay);
                }
            } break;
            case Status::PlayingCDDA:
                playSector();
                break;
            default:
                PCSX::g_system->log(PCSX::LogClass::CDROM, "unsupported yet\n");
                PCSX::g_system->pause();
                break;
        }
    }

    void scheduledDmaCallback() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::DmaComplete, 0, 0, 0);
        auto &mem = PCSX::g_emulator->m_mem;
        if (mem->isDMABusy<3>()) {
            mem->clearDMABusy<3>();
            mem->dmaInterrupt<3>();
        }
    }

    void scheduleFifo(uint32_t cycles) { PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::CDRFIFO, cycles); }
    void scheduleFifo(std::chrono::nanoseconds delay) { scheduleFifo(PCSX::psxRegisters::durationToCycles(delay)); }

    void schedule(uint32_t cycles) { PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::CDRCOMMANDS, cycles); }
    void schedule(std::chrono::nanoseconds delay) { schedule(PCSX::psxRegisters::durationToCycles(delay)); }

    void scheduleRead(uint32_t cycles) { PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::CDREAD, cycles); }
    void scheduleRead(std::chrono::nanoseconds delay) { scheduleRead(PCSX::psxRegisters::durationToCycles(delay)); }

    void scheduleDMA(uint32_t cycles) { PCSX::g_emulator->m_cpu->schedule(PCSX::Schedule::CDRDMA, cycles); }
    void scheduleDMA(std::chrono::nanoseconds delay) { scheduleDMA(PCSX::psxRegisters::durationToCycles(delay)); }

    bool maybeEnqueueResponse(QueueElement &response) {
        const std::array<uint64_t, 2> historyBefore{m_historyQueueIds[2], m_historyQueueIds[3]};
        if (m_responseFifo[0].valueEmpty() && !m_responseFifo[1].empty()) {
            m_responseFifo[0] = m_responseFifo[1];
            m_responseFifo[1] = response;
            historyResponse(response, 3, true, historyBefore);
            return true;
        }
        if (m_responseFifo[0].empty()) {
            m_responseFifo[0] = response;
            historyResponse(response, 1, true, historyBefore);
            return true;
        } else if (m_responseFifo[1].empty()) {
            m_responseFifo[1] = response;
            historyResponse(response, 2, false, historyBefore);
        } else {
            historyResponse(response, 0, false, historyBefore);
        }
        return false;
    }

    // HINTMSK gates the interrupt line, not the cause: a masked cause still lands in HINTSTS,
    // and the interrupt fires when the mask lets it through. The mask applies to the bits of
    // the cause value: on a SCPH-9002, cause 3 is held back by 1Ch and goes through 1Bh.
    void raiseIRQIfUnmasked() {
        const bool assertIRQ = !m_responseFifo[0].valueEmpty() && (m_responseFifo[0].value & m_interruptCauseMask);
        PCSX::History::emit(PCSX::History::Kind::CdIRQ, 1, {m_responseFifo[0].value, m_interruptCauseMask, uint32_t(assertIRQ)}, m_historyQueueIds[2]);
        if (assertIRQ) {
            PCSX::g_emulator->m_mem->setIRQ(4);
        }
    }

    void maybeTriggerIRQ(Cause cause, QueueElement &element) {
        uint8_t causeValue = static_cast<uint8_t>(cause);
        {
            element.setValue(cause);
            bool actuallyTriggering = false;
            if (maybeEnqueueResponse(element)) {
                raiseIRQIfUnmasked();
                actuallyTriggering = (causeValue & m_interruptCauseMask) != 0;
            }
            const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                                   .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
            if (debug) {
                auto &regs = PCSX::g_emulator->m_cpu->m_regs;
                if (actuallyTriggering) {
                    PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] triggering IRQ with cause %d\n",
                                        regs.pc, regs.cycle, causeValue);
                } else {
                    PCSX::g_system->log(PCSX::LogClass::CDROM,
                                        "CD-Rom: %08x.%08x] cause %d queued or masked, no IRQ yet\n",
                                        regs.pc, regs.cycle, causeValue);
                }
            }
        }
    }

    uint8_t getStatus(bool resetLid = false) {
        bool lidOpen = isLidOpen();
        uint8_t v1 = m_motorOn && !lidOpen ? 0x02 : 0;
        // The shell-open bit is reported by the GetStat that clears it (SCPH-9002).
        uint8_t v4 = m_wasLidOpened ? 0x10 : 0;
        if (resetLid && !lidOpen) m_wasLidOpened = false;
        uint8_t v567 = 0;
        switch (m_status) {
            case Status::ReadingData:
                v567 = 0x20;
                break;
            case Status::Seeking:
                v567 = 0x40;
                break;
            case Status::PlayingCDDA:
                v567 = 0x80;
                break;
        }
        return v1 | v4 | v567;
    }

    uint8_t read0() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerRead, 0x1f801800+0, 8);
        // HSTS (host status) register
        /*
          bit 7: BUSYSTS (busy status)
                 This is high when the host writes a command into the command register and low when the sub
                 CPU sets the CLRBUSY bit (bit 6) of the CLRCTL register.
         */
        uint8_t v7 = m_commandFifo.hasValue ? 0x80 : 0;
        /*
          bit 6: DRQSTS (data request status)
                 Indicates to the host that the buffer memory data transfer request status is established. When
                 transferring data in the I/O mode, the host should confirm that this bit is high before accessing the
                 WRDATA or RDDATA register.
         */
        uint8_t v6 = m_dataFIFOSize != m_dataFIFOIndex ? 0x40 : 0;
        /*
          bit 5: RSLRRDY (result read ready)
                 The result register is not empty when this bit is high. At this time, the host can read the result
                 register.
         */
        uint8_t v5 = !m_responseFifo[0].isPayloadAtEnd() ? 0x20 : 0;
        /*
          bit 4: PRMWRDY (parameter write ready)
                 The PARAMETER register is not full when this bit is high. At this time, the host writes data into the
                 PARAMETER register.
         */
        uint8_t v4 = !m_commandFifo.isPayloadFull() ? 0x10 : 0;
        /*
          bit 3: PRMEMPT (parameter empty)
                 The PARAMETER register is empty when this bit is high.
         */
        uint8_t v3 = m_commandFifo.isPayloadEmpty() ? 0x08 : 0;
        /*
          bit 2: ADPBUSY (ADPCM busy)
                 This bit is set high for ADPCM decoding.
         */
        uint8_t v2 = adpcmBusy() ? 0x04 : 0;
        /*
          bits 1, 0: RA1, 0
                 The values of the RA1 and 0 bits for the ADDRESS register can be read from these bits.
         */
        uint8_t v01 = m_registerAddress & 3;

        uint8_t ret = v01 | v2 | v3 | v4 | v5 | v6 | v7;
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] r0: %02x\n", regs.pc, regs.cycle, ret);
        }

        { const uint8_t historyResult = ret; historyScope.result(historyResult); return historyResult; }
    }

    uint8_t read1() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerRead, 0x1f801800+1, 8);
        // RESULT
        uint8_t ret = m_responseFifo[0].readPayloadByte();
        maybeScheduleNextCommand();

        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] r1: %02x\n", regs.pc, regs.cycle, ret);
        }
        { const uint8_t historyResult = ret; historyScope.result(historyResult); return historyResult; }
    }

    uint8_t read2() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerRead, 0x1f801800+2, 8);
        // RD DATA
        uint8_t ret = 0;
        if (!dataFIFOHasData()) {
            ret = 0;
        } else {
            const uint32_t consumedOffset = m_dataFIFOIndex;
            ret = m_dataFIFO[m_dataFIFOIndex++];
            PCSX::History::CD::consume(consumedOffset, 1, &ret, 0x1f801802, false);
            // Draining the whole sector drops the data request: with BFRD left set, the next
            // sector is not loaded into the fifo until the host writes BFRD again.
            if (m_dataFIFOIndex == m_dataFIFOSize) m_dataRequested = false;
        }

        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] r2: %02x\n", regs.pc, regs.cycle, ret);
        }
        { const uint8_t historyResult = ret; historyScope.result(historyResult); return historyResult; }
    }

    uint8_t read3() override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerRead, 0x1f801800+3, 8);
        /*
         * bit 4: BFWRDY (buffer write ready)
         *        The BFWRDY status is established if there is area where writing is possible in the buffer of 1 sector
         *        or more for sound map playback. It is established in any of the following cases:
         *        (1) When the host has set the SMEN bit (bit 5) of the HCHPCTL register high
         *        (2) When there is sound map data area of 1 sector or more in the buffer memory (when the buffer
         *            is not full) after the sound map data equivalent to 1 sector from the host has been written into
         *            the buffer memory
         *        (3) When an area for writing the sound map data has been created in the buffer memory by the
         *            completion of the sound map ADPCM decoding of one sector
         * bit 3: BFEMPT (buffer empty)
         *        The BFEMPT status is established when there is no more sector data in the buffer memory upon
         *        completion of the sound map ADPCM decoding of one sector for sound map playback.
         * bits 2 to 0: INTSTS#2 to 0
         *        The values of these bits are those of the corresponding bits for the sub CPU HIFCTL register.
         */

        uint8_t ret = 0;
        switch (m_registerAddress & 1) {
            case 0: {
                // HINT MSK (host interrupt mask)
                ret = m_interruptCauseMask;
            } break;
            case 1: {
                // HINT STS (host interrupt status)
                ret = m_responseFifo[0].getValue();
            } break;
        }
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] r3.%i: %02x\n", regs.pc, regs.cycle,
                                m_registerAddress & 1, ret);
        }
        { const uint8_t historyResult = ret | 0xe0; historyScope.result(historyResult); return historyResult; }
    }

    void write0(uint8_t value) override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerWrite, 0x1f801800+0, 8, value);
        // ADDRESS
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] w0: %02x\n", regs.pc, regs.cycle, value);
        }
        m_registerAddress = value & 3;
    }

    void write1(uint8_t value) override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerWrite, 0x1f801800+1, 8, value);
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] w1.%i: %02x\n", regs.pc, regs.cycle,
                                m_registerAddress, value);
        }
        switch (m_registerAddress) {
            case 0:
                // COMMAND
                m_commandFifo.value = value;
                PCSX::History::CD::submission(); historyQueueChanged(0);
                if (!m_commandFifo.hasValue) {
                    if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                            .get<PCSX::Emulator::DebugSettings::LoggingCDROM>()) {
                        logCDROM(m_commandFifo);
                    }
                    scheduleFifo(797us);
                }
                m_commandFifo.hasValue = true;
                break;
            case 1: {
                // WR DATA
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: w1:1 not available yet\n");
                PCSX::g_system->pause();
            } break;
            case 2: {
                /*
                CI (coding information)
                This sets the coding information for sound map playback. The bit allocation is the same as that for the
                coding information bytes of the sub header.
                  bits 7, 5, 3, 1: Reserved
                  bit 6: EMPHASIS
                      High: Emphasis ON
                      Low : Emphasis OFF
                  bit 4: BITLNGTH
                      High: 8 bits
                      Low : 4 bits
                  bit 2: FS
                      High: 18.9 kHz
                      Low : 37.8 kHz
                  bit 0: S/M (stereo/mono)
                      High: Stereo
                      Low : Mono
                */
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: w1:2 not available yet\n");
                PCSX::g_system->pause();
            } break;
            case 3: {
                // ATV2 Right-to-Right
                m_atvPending[2] = value;
            } break;
        }
    }

    void write2(uint8_t value) override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerWrite, 0x1f801800+2, 8, value);
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] w2.%i: %02x\n", regs.pc, regs.cycle,
                                m_registerAddress, value);
        }
        switch (m_registerAddress) {
            case 0: {
                // PARAMETER
                m_commandFifo.pushPayloadData(value);
                historyQueueChanged(0);
            } break;
            case 1: {
                // HINT MSK (host interrupt mask)
                m_interruptCauseMask = value;
                raiseIRQIfUnmasked();
            } break;
            case 2: {
                // ATV0 Left-to-Left
                m_atvPending[0] = value;
            } break;
            case 3: {
                // ATV3 Right-to-Left
                m_atvPending[3] = value;
            } break;
        }
    }

    void write3(uint8_t value) override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::HandlerWrite, 0x1f801800+3, 8, value);
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingHWCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] w3.%i: %02x\n", regs.pc, regs.cycle,
                                m_registerAddress, value);
        }
        switch (m_registerAddress) {
            case 0: {
                // clang-format off
                /*
                HCHPCTL (host chip control) register
                  bit 7: BFRD (buffer read)
                         The transfer of (drive) data from the buffer memory to the host is started by setting this bit high.
                         The bit is automatically set low upon completion of the transfer.
                  bit 6: BFWR (buffer write)
                         The transfer of data from the host to the buffer memory is started by setting this bit high. The bit is
                         automatically set low upon completion of the transfer.
                  bit 5: SMEN (sound map En)
                         This is set high to perform sound map ADPCM playback.
                */
                // clang-format on
                bool bfrd = value & 0x80;
                bool bfwr = value & 0x40;
                bool smen = value & 0x20;
                if (bfrd) {
                    m_dataRequested = true;
                    m_dataFIFOSize = m_dataFIFOPending;
                } else {
                    m_dataRequested = false;
                    m_dataFIFOSize = 0;
                    m_dataFIFOIndex = 0;
                }
                m_soundMapEnabled = smen;
            } break;
            case 1: {
                // clang-format off
                /*
                HCLRCTL (host clear control)
                When each bit of this register is set high, the chip, status, register, interrupt status and interrupt request to
                the host generated by the status are cleared.
                  bit 7: CHPRST (chip reset)
                         The inside of the IC is initialized by setting this bit high. The bit is automatically set low upon
                         completion of the initialization of the IC. There is therefore no need for the host to reset low. When
                         the inside of the IC is initialized by setting bit high, the XHRS pin is set low.
                  bit 6: CLRPRM (clear parameter)
                         The parameter register is cleared by setting this bit high. The bit is automatically set low upon
                         completion of the clearing for the parameter register. There is therefore no need for the host to
                         reset low.
                  bit 5: SMADPCLR (sound map ADPCM clear)
                         This bit is set high to terminate sound map ADPCM decoding forcibly.
                         (1) When this bit has been set high for sound map ADPCM playback (when both SMEN and
                             ADPBSY (HSTS register bit 2) are high):
                             • ADPCM decoding during playback is suspended. (Noise may be generated).
                             • The sound map and buffer management circuits in the IC are cleared, making the buffer
                               empty. The BFEMPT interrupt status is established.
                             (Note) Set the SMEN bit low at the same time as this bit is set high.
                         (2) Setting this bit high when the sound map ADPCM playback is not being performed has no
                             effect whatsoever
                  bit 4: CLRBFWRDY (clear buffer write ready interrupt)
                  bit 3: CLRBFEMPT (clear buffer write empty interrupt)
                  bits 2 to 0: CLRINT#2 to 0 (clear interrupt #2 to 0)
                      bit 4 clears the corresponding interrupt status.
                */
                // clang-format on
                bool ack = false;
                // cause ack
                if (value == 0x07) {
                    // partial ack?
                    ack = true;
                }
                if (value == 0x1f) {
                    // all ack?
                    ack = true;
                }
                if (ack) {
                    if (debug) {
                        PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: acked %02x (was %i)\n", value,
                                            m_responseFifo[0].value);
                    }
                    m_responseFifo[0].valueRead = true;
                    maybeScheduleNextCommand();
                    return;
                }
                if (value & 0x10) {
                    // request ack?
                    // TODO: act on this?
                }
                if (value & 0x08) {
                    // ??
                    // TODO: act on this?
                }
                if (value & 0x40) {
                    m_commandFifo.payloadSize = 0;
                    return;
                }
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: w3:1(%02x) not available yet\n", value);
                PCSX::g_system->pause();
            } break;
            case 2: {
                // ATV1 Left-to-Right
                m_atvPending[1] = value;
            } break;
            case 3: {
                // clang-format off
                /*
                ADPCTL (ADPCM control) register
                  bit 5: CHNGATV (change ATV register)
                         The host sets this bit high after the changes of the ATV 3 to 0 registers have been completed. The
                         attenuator value in the IC is switched for the first time. There is no need for the host to set this bit
                         low. The bit used to set the ATV3 to 0 registers of the host and to synchronize the IC audio
                         playback.
                  bit 0: ADPMUTE (ADPCM mute)
                         Set high to mute the ADPCM sound for ADPCM decoding.
                  bits 7, 6, 4 to 1: Reserved
                */
                // clang-format on
                if (value & 0x20) memcpy(m_atv, m_atvPending, sizeof(m_atv));
                m_adpcmMuted = value & 0x01;
            } break;
        }
    }

    void dma(uint32_t madr, uint32_t bcr, uint32_t chcr) override {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Dma, madr, 32, chcr);
        uint32_t size = bcr & 0xffff;
        size *= 4;
        if (size == 0) size = 0xffffffff;
        size = std::min(m_dataFIFOSize - m_dataFIFOIndex, size);
        PCSX::IO<PCSX::File> memFile = PCSX::g_emulator->m_mem->getMemoryAsFile();
        memFile->wSeek(madr);
        PCSX::History::CD::consume(m_dataFIFOIndex, size, m_dataFIFO + m_dataFIFOIndex, madr, true);
        PCSX::History::CD::dmaBegin(madr, m_dataFIFOIndex, size);
        const auto written = memFile->write(m_dataFIFO + m_dataFIFOIndex, size);
        PCSX::History::CD::dmaEnd(written);
        m_dataFIFOIndex += size;
        if ((size != 0) && (m_dataFIFOIndex == m_dataFIFOSize)) m_dataRequested = false;
        PCSX::g_emulator->m_cpu->Clear(madr, size / 4);
        PCSX::g_emulator->m_mem->msanDmaWrite(madr, size);
        if (PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                .get<PCSX::Emulator::DebugSettings::Debug>()) {
            PCSX::g_emulator->m_debug->checkDMAwrite(3, madr, size);
        }
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
        if (debug) {
            auto &regs = PCSX::g_emulator->m_cpu->m_regs;
            PCSX::g_system->log(PCSX::LogClass::CDROM,
                                "CD-Rom: %08x.%08x] DMA transfer requested to address %08x, size %08x\n", regs.pc,
                                regs.cycle, madr, size);
        }
        if (chcr == 0x11400100) {
            scheduleDMA(size / 16);
        } else {
            scheduleDMA(size / 4);
        }
    }

    void maybeEnqueueError(uint8_t mask1, uint8_t mask2) {
        QueueElement error;
        error.pushPayloadData(getStatus() | mask1);
        error.pushPayloadData(mask2);
        maybeTriggerIRQ(Cause::Error, error);
    }

    bool invokeCommand(bool (CDRomImpl::*handler)(const QueueElement&, bool), const QueueElement& command, bool start) {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Command, command.value, 0, start);
        const auto queue = historyQueue(command, start ? m_historyQueueIds[0] : m_historyQueueIds[1]);
        PCSX::History::emit(PCSX::History::Kind::CdCommand, 1, {command.value, uint32_t(start), command.payloadSize},
            PCSX::History::CD::access(), queue.data(), queue.size(), historyScope.id());
        const bool result = (this->*handler)(command, start);
        historyScope.result(result);
        PCSX::History::emit(PCSX::History::Kind::CdCommand, 2, {command.value, uint32_t(start), uint32_t(result)},
            PCSX::History::CD::access(), nullptr, 0, historyScope.id());
        return result;
    }

    void maybeStartCommand() {
        if (m_commandFifo.empty()) return;
        auto command = m_commandFifo.value;
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
        if (!m_responseFifo[0].valueEmpty() || !m_responseFifo[1].empty()) {
            if (debug) {
                PCSX::g_system->log(PCSX::LogClass::CDROM,
                                    "CD-Rom: command %s (%i) pending, but response fifo full; won't start.\n",
                                    commandName(command), command);
            }
            return;
        }
        if (debug) {
            PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: Starting command %s (%i)\n", commandName(command),
                                command);
        }
        static constexpr unsigned c_commandMax = sizeof(c_commandsArgumentsCount) / sizeof(c_commandsArgumentsCount[0]);
        if (command >= c_commandMax) {
            maybeEnqueueError(1, 0x40);
            maybeScheduleNextCommand();
            m_commandFifo.clear();
            return;
        }
        auto expectedCount = c_commandsArgumentsCount[command];
        if ((expectedCount >= 0) && (expectedCount != m_commandFifo.payloadSize)) {
            maybeEnqueueError(1, 0x20);
            maybeScheduleNextCommand();
            m_commandFifo.clear();
            return;
        }
        auto handler = c_commandsHandlers[command];
        if (handler) {
            if (invokeCommand(handler, m_commandFifo, true)) { m_commandExecuting = m_commandFifo; m_historyQueueIds[1] = m_historyQueueIds[0]; }
        } else {
            // 17h and 18h are unused opcodes and error out like 00h. Anything else without a
            // handler is a real command that is not emulated yet.
            if ((command != 0x17) && (command != 0x18)) {
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: Unsupported command %i (%s).\n", command,
                                    commandName(command));
                PCSX::g_system->pause();
            }
            maybeEnqueueError(1, 0x40);
            maybeScheduleNextCommand();
        }
        m_commandFifo.clear();
    }

    void maybeScheduleNextCommand() {
        const bool debug = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingDebugSettings>()
                               .get<PCSX::Emulator::DebugSettings::LoggingCDROM>();
        if (!responseFifoFull()) {
            if (debug) {
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: Scheduling queued next command to run.\n");
            }
            scheduleFifo(797us);
        } else if (debug) {
            PCSX::g_system->log(PCSX::LogClass::CDROM,
                                "CD-Rom: Won't schedule next command to run as response fifo is full.\n");
        }
    }

    enum class SeekType { DATA, CDDA };

    std::chrono::nanoseconds computeSeekDelay(MSF from, MSF to, SeekType seekType, bool forReading = false) {
        unsigned destTrack = m_iso->getTrack(to);
        if (destTrack == 0) return 650ms;
        if ((seekType == SeekType::DATA) && (m_iso->getTrackType(destTrack) == PCSX::CDRIso::TrackType::CDDA)) {
            return 4s;
        }
        uint32_t distance = 0;
        if (from > to) {
            distance = (from - to).toLBA();
        } else {
            distance = (to - from).toLBA();
        }
        // TODO: ought to be a decent approximation for now,
        // but may require some tuning later on.
        return std::chrono::microseconds(distance * 3) + (forReading ? 1ms : 167ms);
    }

    std::chrono::nanoseconds computeReadDelay() { return m_speed == Speed::Simple ? 13333us : 6666us; }

    // "Command" 0, which doesn't actually exist.
    bool cdlSync(const QueueElement &command, bool start) {
        maybeEnqueueError(1, 0x40);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 1.
    bool cdlNop(const QueueElement &command, bool start) {
        QueueElement response;
        response.pushPayloadData(getStatus(true));
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 2.
    bool cdlSetLoc(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        // What happens when issued during Read / Play?
        auto maybeMSF = getMSF(command.payload);
        QueueElement response;
        Cause cause;
        if (maybeMSF.has_value()) {
            response.pushPayloadData(getStatus());
            cause = Cause::Acknowledge;
            maybeTriggerIRQ(cause, response);
            m_seekPosition = maybeMSF.value();
            m_setLocPending = true;
            PCSX::g_emulator->m_cdromLogger->recordAccess(m_seekPosition.toLBA(),
                                                          PCSX::CDRomLogger::AccessType::Seek,
                                                          PCSX::g_emulator->m_cpu->m_regs.cycle);
        } else {
            maybeEnqueueError(1, 0x10);
        }
        maybeScheduleNextCommand();
        return false;
    }

    // Command 3. The optional parameter is a track number: 1..N starts that track, N+1..99 restarts the
    // current one, and none or 0 plays from a pending Setloc, or from the current position.
    bool cdlPlay(const QueueElement &command, bool start) {
        unsigned track = 0;
        if (command.payloadSize > 0) {
            track = isValidBCD(command.payload[0]) ? PCSX::IEC60908b::btoi(command.payload[0]) : 100;
        }
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();

        if (track != 0) {
            if (track > m_iso->getTN()) track = m_iso->getTrack(m_currentPosition);
            if (track != 0) {
                m_seekPosition = m_iso->getTD(track);
                m_setLocPending = true;
            }
        }
        m_readingType = ReadingType::Play;
        if (m_setLocPending) {
            m_setLocPending = false;
            m_status = Status::Idle;
            m_readingState = ReadingState::Seeking;
            scheduleRead(20ms);
        } else {
            m_readingState = ReadingState::None;
            startPlaying();
            scheduleRead(computeReadDelay());
        }
        return false;
    }

    void resetXA() {
        xa_decode_reset(&m_xa);
        m_xaFormatReady = false;
        m_xaFirstSector = true;
        m_xaEnded = false;
    }

    bool adpcmBusy() const { return m_realtime && (m_status == Status::ReadingData) && !m_xaEnded; }

    static int16_t saturate(int v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

    // CD-DA through the ATV matrix (ATV0 L->L, ATV1 L->R, ATV2 R->R, ATV3 R->L, 0x80 = unity),
    // with the levels a SCPH-9002 gives on its SPU CD capture: 0.950 before the 16-bit clamp and
    // 0.973 after it. A full-scale sine at unity comes out at 0.924, and the clamped level sits
    // at 31880.
    void attenuateCDDA(int16_t *buf, int frames) {
        const int ll = m_atv[0], lr = m_atv[1], rr = m_atv[2], rl = m_atv[3];
        for (int i = 0; i < frames; i++) {
            int64_t l = buf[i * 2];
            int64_t r = buf[i * 2 + 1];
            int outL = saturate(int(((l * ll + r * rl) * 31130) >> 22));
            int outR = saturate(int(((r * rr + l * lr) * 31130) >> 22));
            buf[i * 2] = (outL * 31880) >> 15;
            buf[i * 2 + 1] = (outR * 31880) >> 15;
        }
    }

    void playXASector(uint8_t *subHeader, const PCSX::IEC60908b::SubHeaders &subHeaders) {
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::NativeBranch, {1, uint32_t(m_xaEnded)});
        if (m_xaEnded) return;
        const bool filtered = m_subheaderFilter && ((subHeader[0] != m_filterFile) || (subHeader[1] != m_filterChannel));
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::NativeBranch,
            {2, uint32_t(filtered), uint32_t(m_subheaderFilter), subHeader[0], subHeader[1], m_filterFile, m_filterChannel});
        if (filtered) return;
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::NativeBranch, {3, uint32_t(subHeader[1] == 0xff), subHeader[1]});
        if (subHeader[1] == 0xff) return;
        const int decoded = xa_decode_sector(&m_xa, subHeader, m_xaFirstSector);
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::XaDecode,
            {uint32_t(decoded), uint32_t(m_xaFirstSector), uint32_t(subHeader[0]), uint32_t(subHeader[1]), 0, 1});
        if (decoded == 0) {
            PCSX::History::CD::dataMutation(6, 0, m_xa.nsamples * (m_xa.stereo ? 2 : 1) * sizeof(m_xa.pcm[0]), m_xa.pcm);
            PCSX::History::CD::predicate(PCSX::History::CD::Predicate::XaDecode,
                {0, uint32_t(m_xa.freq), uint32_t(m_xa.nbits), uint32_t(m_xa.stereo), uint32_t(m_xa.nsamples), 2});
            m_xaFormatReady = true;
            m_xaFirstSector = false;
            PCSX::History::CD::predicate(PCSX::History::CD::Predicate::NativeBranch,
                {4, uint32_t(!m_muted && !m_adpcmMuted), uint32_t(m_muted), uint32_t(m_adpcmMuted)});
            if (!m_muted && !m_adpcmMuted) {
                // Mono feeds the same sample to both matrix inputs (SCPH-9002: L gets ATV0+ATV3,
                // R gets ATV1+ATV2).
                m_xaOut.freq = m_xa.freq;
                m_xaOut.nbits = m_xa.nbits;
                m_xaOut.stereo = 1;
                m_xaOut.nsamples = m_xa.nsamples;
                if (m_xa.stereo) {
                    memcpy(m_xaOut.pcm, m_xa.pcm, m_xa.nsamples * 2 * sizeof(m_xa.pcm[0]));
                } else {
                    for (int i = 0; i < m_xa.nsamples; i++) m_xaOut.pcm[i * 2] = m_xaOut.pcm[i * 2 + 1] = m_xa.pcm[i];
                }
                PCSX::g_emulator->m_spu->setXAVolume(m_atv);
                PCSX::History::CD::dataMutation(7, 0, m_xaOut.nsamples * 2 * sizeof(m_xaOut.pcm[0]), m_xaOut.pcm);
                const auto ids = PCSX::History::CD::identities();
                PCSX::History::CD::Feed historyFeed(false, ids[4], ids[3], ids[2], m_xaOut.freq, m_xaOut.nsamples, m_xaOut.stereo);
                PCSX::g_emulator->m_spu->playADPCMchannel(&m_xaOut);
            }
        }
        // A sector with both EOF and EOR stops ADPCM playback, with no interrupt; reading carries on.
        // EOF alone or EOR alone does not (#6, SCPH-9002).
        const bool ended = subHeaders.isEOF() && subHeaders.isEndOfRecord();
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::NativeBranch, {5, uint32_t(ended), uint32_t(subHeaders.isEOF()), uint32_t(subHeaders.isEndOfRecord())});
        if (ended) m_xaEnded = true;
    }

    void startPlaying() {
        m_status = Status::PlayingCDDA;
        m_playTrack = 0;
        m_playStartCycle = PCSX::g_emulator->m_cpu->m_regs.cycle;
    }

    void stopPlaying() {
        m_status = Status::Idle;
        m_readingType = ReadingType::None;
        QueueElement end;
        end.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::End, end);
    }

    // One sector of CD-DA playback: audio to the SPU, then the optional report.
    void playSector() {
        PCSX::History::CD::Scope historyScope(PCSX::History::CD::Operation::Play, 0, 0, 0);
        const bool endOfDisc = m_currentPosition >= m_iso->getTD(0);
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback, {1, uint32_t(endOfDisc)});
        if (endOfDisc) {
            // End of disc: an end cause, and the motor stops.
            stopPlaying();
            m_motorOn = false;
            return;
        }
        uint8_t locP[8];
        bool haveQ = m_iso->getLocP(m_currentPosition, locP);
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback, {2, uint32_t(haveQ)});
        if (haveQ) {
            if (m_playTrack == 0) m_playTrack = locP[0];
            const bool autoPause = m_autoPause && (locP[0] != m_playTrack);
            PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback, {3, uint32_t(autoPause), uint32_t(m_autoPause), locP[0], m_playTrack});
            if (autoPause) {
                // Autopause: an end cause on the SubQ track transition, staying where we are.
                stopPlaying();
                return;
            }
        }

        int16_t samples[PCSX::IEC60908b::FRAMESIZE_RAW / 2];
        PCSX::History::CD::Media historyMedium(2, uint32_t(m_currentPosition.m) | (uint32_t(m_currentPosition.s)<<8) | (uint32_t(m_currentPosition.f)<<16), m_currentPosition.toLBA(), PCSX::History::CD::backing());
        const bool audioReadResult = m_iso->readCDDA(m_currentPosition, reinterpret_cast<uint8_t *>(samples));
        historyMedium.result(audioReadResult, 0);
        PCSX::History::CD::dataMutation(5, 0, sizeof(samples), samples, historyMedium.id());
        PCSX::g_emulator->m_cdromLogger->recordAccess(m_currentPosition.toLBA(), PCSX::CDRomLogger::AccessType::Data,
                                                      PCSX::g_emulator->m_cpu->m_regs.cycle);
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback, {4, uint32_t(!m_muted)});
        if (!m_muted) {
            int16_t out[std::size(samples)];
            memcpy(out, samples, sizeof(out));
            attenuateCDDA(out, std::size(out) / 2);
            const auto ids = PCSX::History::CD::identities();
            PCSX::History::CD::Feed historyFeed(true, ids[4], ids[3], ids[2], 44100, sizeof(out)/4, 1);
            PCSX::g_emulator->m_spu->playCDDAchannel(out, sizeof(out));
        }

        // The peak's L/R flag toggles on every SubQ read, i.e. every sector.
        m_peakFlag = !m_peakFlag;
        const uint32_t elapsed = PCSX::g_emulator->m_cpu->m_regs.cycle - m_playStartCycle;
        const bool settled = elapsed >= PCSX::psxRegisters::durationToCycles(750ms);
        const bool reportEligible = m_report && haveQ && settled;
        PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback,
            {5, uint32_t(reportEligible), uint32_t(m_report), uint32_t(haveQ), uint32_t(settled), elapsed});
        if (reportEligible) {
            uint8_t frame = PCSX::IEC60908b::btoi(locP[7]);
            PCSX::History::CD::predicate(PCSX::History::CD::Predicate::Playback, {6, uint32_t((frame % 10) == 0), frame});
            if ((frame % 10) == 0) {
                unsigned peak = 0;
                for (unsigned i = m_peakFlag ? 1 : 0; i < std::size(samples); i += 2) {
                    int s = samples[i];
                    unsigned a = s < 0 ? -s : s;
                    if (a > peak) peak = a;
                }
                if (peak > 0x7fff) peak = 0x7fff;
                if (m_peakFlag) peak |= 0x8000;
                QueueElement report;
                report.pushPayloadData(getStatus());
                report.pushPayloadData(locP[0]);
                report.pushPayloadData(locP[1]);
                if (((frame / 10) & 1) == 0) {
                    report.pushPayloadData(locP[5]);
                    report.pushPayloadData(locP[6]);
                    report.pushPayloadData(locP[7]);
                } else {
                    report.pushPayloadData(locP[2]);
                    report.pushPayloadData(locP[3] | 0x80);
                    report.pushPayloadData(locP[4]);
                }
                report.pushPayloadData(peak & 0xff);
                report.pushPayloadData(peak >> 8);
                maybeTriggerIRQ(Cause::DataReady, report);
            }
        }
        m_currentPosition++;
        scheduleRead(computeReadDelay());
    }

    // Commands 4 and 5. Only valid while playing audio; otherwise the drive errors out.
    bool cdlForward(const QueueElement &command, bool start) { return fastSkip(command, start); }
    bool cdlBackward(const QueueElement &command, bool start) { return fastSkip(command, start); }
    bool fastSkip(const QueueElement &command, bool start) {
        if (m_status != Status::PlayingCDDA) {
            maybeEnqueueError(1, 0x80);
        } else {
            // TODO: skip sectors while playing, once CdlPlay exists.
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
        }
        maybeScheduleNextCommand();
        return false;
    }

    // Command 6.
    bool cdlReadN(const QueueElement &command, bool start) {
        // A read sent while a SeekP is still running takes over: the seek never completes.
        if (!m_commandExecuting.empty() && (m_commandExecuting.value == 22)) {
            PCSX::g_emulator->m_cpu->unschedule(PCSX::Schedule::CDRCOMMANDS);
            m_commandExecuting.clear();
        }
        m_setLocPending = false;
        resetXA();
        m_status = Status::Idle;
        scheduleRead(20ms);
        m_readingType = ReadingType::Normal;
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        m_readingState = ReadingState::Seeking;
        return false;
    }

    // Command 7. Spins the motor up from stopped; on a spinning drive it is an error.
    // SCPH-9002: completes about 2 s after the acknowledge.
    bool cdlStandby(const QueueElement &command, bool start) {
        if (start) {
            if (m_motorOn) {
                maybeEnqueueError(1, 0x20);
                maybeScheduleNextCommand();
                return false;
            }
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(2000ms);
            return true;
        } else {
            m_motorOn = true;
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 8. Stops reading or playing and the motor. SCPH-9002: completes about
    // 400 ms after the acknowledge when spinning, under 2 ms when already stopped.
    bool cdlStop(const QueueElement &command, bool start) {
        if (start) {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(m_motorOn ? 400ms : 1700us);
            m_status = Status::Idle;
            m_readingState = ReadingState::None;
            m_readingType = ReadingType::None;
            m_invalidLocL = true;
            m_motorOn = false;
            return true;
        } else {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 18. Only single-session images are emulated. SCPH-9002 with a single-session
    // disc: session 0 errors with 10h, session 1 completes about 2.6 s after the acknowledge,
    // and session 2 acknowledges and sends nothing else for at least 8 s.
    bool cdlSetSession(const QueueElement &command, bool start) {
        if (start) {
            auto session = command.payload[0];
            if (session == 0) {
                maybeEnqueueError(1, 0x10);
                maybeScheduleNextCommand();
                return false;
            }
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            if (session != 1) {
                maybeScheduleNextCommand();
                return false;
            }
            schedule(2600ms);
            return true;
        } else {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 29. SCPH-9002 with the test disc: acknowledges, and no second response comes
    // within 8 s for track 1, track 2 or the lead-out, so only the acknowledge is emulated.
    bool cdlGetQ(const QueueElement &command, bool start) {
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 28. SCPH-9002: acknowledges with no second response, then reports mode 0x20
    // and the shell-open status bit, which stays set until a GetStat.
    bool cdlReset(const QueueElement &command, bool start) {
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        applyMode(0x20);
        m_wasLidOpened = true;
        maybeScheduleNextCommand();
        return false;
    }

    // Command 30. Re-reads the table of contents. SCPH-9002: completes about 760 ms
    // after the acknowledge, with the motor still on.
    bool cdlReadTOC(const QueueElement &command, bool start) {
        if (start) {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(760ms);
            m_status = Status::Idle;
            m_readingState = ReadingState::None;
            m_readingType = ReadingType::None;
            m_invalidLocL = true;
            return true;
        } else {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 9.
    bool cdlPause(const QueueElement &command, bool start) {
        if (start) {
            if (m_status == Status::Idle) {
                schedule(200us);
            } else {
                schedule(m_speed == Speed::Simple ? 70ms : 35ms);
            }
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            m_status = Status::Idle;
            m_readingState = ReadingState::None;
            m_invalidLocL = true;
            return true;
        } else {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 10.
    bool cdlInit(const QueueElement &command, bool start) {
        if (start) {
            QueueElement response;
            m_motorOn = true;
            m_speedChanged = false;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(120ms);
            // TODO: figure out exactly the various states of the CD-Rom controller
            // that are being reset, and their value.
            m_currentPosition.reset();
            m_currentPosition.s = 2;
            m_seekPosition.reset();
            m_seekPosition.s = 2;
            m_invalidLocL = false;
            m_speed = Speed::Simple;
            m_status = Status::Idle;
            m_interruptCauseMask = 0x1f;
            m_readingState = ReadingState::None;
            m_readingType = ReadingType::None;
            m_setLocPending = false;
            m_autoPause = false;
            m_report = false;
            memset(m_lastLocP, 0, sizeof(m_lastLocP));
            // Probably need to cancel other scheduled tasks here.
            return true;
        } else {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 11
    bool cdlMute(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        m_muted = true;
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 12
    bool cdlDemute(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        m_muted = false;
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 13
    bool cdlSetFilter(const QueueElement &command, bool start) {
        m_filterFile = command.payload[0];
        m_filterChannel = command.payload[1];
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 14
    bool cdlSetMode(const QueueElement &command, bool start) {
        applyMode(command.payload[0]);
        QueueElement response;
        response.pushPayloadData(getStatus());
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    void applyMode(uint8_t mode) {
        // TODO: add the rest of the mode bits.
        if (mode & 0x80) {
            if (m_speed == Speed::Simple) {
                m_speed = Speed::Double;
                m_speedChanged = true;
            }
        } else {
            if (m_speed == Speed::Double) {
                m_speed = Speed::Simple;
                m_speedChanged = true;
            }
        }
        switch ((mode & 0x30) >> 4) {
            case 0:
                m_readSpan = ReadSpan::S2048;
                break;
            case 1:
                m_readSpan = ReadSpan::S2328;
                break;
            case 2:
                m_readSpan = ReadSpan::S2340;
                break;
            case 3:
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: unsupported mode: %02x\n", mode);
                PCSX::g_system->pause();
                break;
        }
        m_subheaderFilter = (mode & 0x08) != 0;
        bool realtime = (mode & 0x40) != 0;
        if (realtime && !m_realtime) resetXA();
        m_realtime = realtime;
        m_report = (mode & 0x04) != 0;
        m_autoPause = (mode & 0x02) != 0;
        // Bit 0 (CDDA) only matters for reading audio sectors as data, which is not modelled; keep it
        // for GetParam.
        m_mode = mode;
    }

    // Command 15.
    bool cdlGetParam(const QueueElement &command, bool start) {
        QueueElement response;
        response.pushPayloadData(getStatus());
        response.pushPayloadData(m_mode);
        response.pushPayloadData(0);
        response.pushPayloadData(m_filterFile);
        response.pushPayloadData(m_filterChannel);
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 16.
    bool cdlGetLocL(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        if (m_invalidLocL) {
            maybeEnqueueError(1, 0x80);
        } else {
            QueueElement response;
            response.pushPayloadData(std::string_view((char *)m_lastLocL, sizeof(m_lastLocL)));
            maybeTriggerIRQ(Cause::Acknowledge, response);
        }
        maybeScheduleNextCommand();
        return false;
    }

    // Command 17.
    bool cdlGetLocP(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        m_iso->getLocP(m_currentPosition, m_lastLocP);
        QueueElement response;
        response.pushPayloadData(std::string_view((char *)m_lastLocP, sizeof(m_lastLocP)));
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 19.
    bool cdlGetTN(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        QueueElement response;
        response.pushPayloadData(getStatus());
        response.pushPayloadData(1);
        response.pushPayloadData(PCSX::IEC60908b::itob(m_iso->getTN()));
        maybeTriggerIRQ(Cause::Acknowledge, response);
        maybeScheduleNextCommand();
        return false;
    }

    // Command 20.
    bool cdlGetTD(const QueueElement &command, bool start) {
        // TODO: probably should error out if no disc or
        // lid open?
        auto track = PCSX::IEC60908b::btoi(command.payload[0]);
        if (!isValidBCD(command.payload[0]) || (track > m_iso->getTN())) {
            maybeEnqueueError(1, 0x10);
        } else {
            auto td = m_iso->getTD(track);
            QueueElement response;
            response.pushPayloadData(getStatus());
            response.pushPayloadData(PCSX::IEC60908b::itob(td.m));
            response.pushPayloadData(PCSX::IEC60908b::itob(td.s));
            maybeTriggerIRQ(Cause::Acknowledge, response);
        }
        maybeScheduleNextCommand();
        return false;
    }

    // Command 21.
    bool cdlSeekL(const QueueElement &command, bool start) {
        m_readingState = ReadingState::None;
        m_setLocPending = false;
        if (start) {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(15ms);
            return true;
        } else if (m_status != Status::Seeking) {
            auto seekDelay = computeSeekDelay(m_currentPosition, m_seekPosition, SeekType::DATA);
            if (m_speedChanged) {
                m_speedChanged = false;
                seekDelay += 650ms;
            }
            m_status = Status::Seeking;
            schedule(seekDelay);
            return true;
        } else {
            m_status = Status::Idle;
            m_currentPosition = m_seekPosition;
            unsigned track = m_iso->getTrack(m_seekPosition);
            if (m_iso->getTrackType(track) == PCSX::CDRIso::TrackType::CDDA) {
                maybeEnqueueError(4, 4);
            } else if (track == 0) {
                maybeEnqueueError(4, 0x10);
            } else {
                QueueElement response;
                response.pushPayloadData(getStatus());
                maybeTriggerIRQ(Cause::Complete, response);
            }
            m_invalidLocL = true;
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 22.
    bool cdlSeekP(const QueueElement &command, bool start) {
        m_readingState = ReadingState::None;
        m_setLocPending = false;
        if (start) {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(15ms);
            return true;
        } else if (m_status != Status::Seeking) {
            auto seekDelay = computeSeekDelay(m_currentPosition, m_seekPosition, SeekType::CDDA);
            if (m_speedChanged) {
                m_speedChanged = false;
                seekDelay += 650ms;
            }
            m_status = Status::Seeking;
            schedule(seekDelay);
            return true;
        } else {
            m_status = Status::Idle;
            MSF fudge = m_seekPosition - MSF{m_seekPosition.toLBA() / 32768};
            m_currentPosition = fudge;
            if (m_iso->getTrack(m_seekPosition) == 0) {
                maybeEnqueueError(4, 0x10);
            } else {
                QueueElement response;
                response.pushPayloadData(getStatus());
                maybeTriggerIRQ(Cause::Complete, response);
            }
            m_invalidLocL = true;
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 25.
    bool cdlTest(const QueueElement &command, bool start) {
        // Controller date and version, from the SCPH-9002 the rest of this file is measured on
        // (1 Feb 1999, vC3).
        static constexpr uint8_t c_test20[] = {0x99, 0x02, 0x01, 0xc3};
        if (command.isPayloadEmpty()) {
            maybeEnqueueError(1, 0x20);
            maybeScheduleNextCommand();
            return false;
        }

        switch (command.payload[0]) {
            case 0x20:
                if (command.payloadSize == 1) {
                    QueueElement response;
                    response.pushPayloadData(std::string_view((const char *)c_test20, sizeof(c_test20)));
                    maybeTriggerIRQ(Cause::Acknowledge, response);
                } else {
                    maybeEnqueueError(1, 0x20);
                }
                break;
            case 0x22: {
                // Region string, plain text with no status byte. A SCPH-9002 answers "for Europe";
                // follow the emulated video standard for the rest.
                const bool pal = PCSX::g_emulator->settings.get<PCSX::Emulator::SettingVideo>() ==
                                 PCSX::Emulator::PSX_TYPE_PAL;
                QueueElement response;
                response.pushPayloadData(pal ? std::string_view("for Europe") : std::string_view("for U/C"));
                maybeTriggerIRQ(Cause::Acknowledge, response);
                break;
            }
            case 0x23:
            case 0x24: {
                // Servo amplifier and signal processor chip names; both "CXD2940Q" on a SCPH-9002.
                QueueElement response;
                response.pushPayloadData(std::string_view("CXD2940Q"));
                maybeTriggerIRQ(Cause::Acknowledge, response);
                break;
            }
            default:
                maybeEnqueueError(1, 0x10);
                break;
        }
        maybeScheduleNextCommand();
        return false;
    }

    // Command 26.
    bool cdlID(const QueueElement &command, bool start) {
        if (start) {
            QueueElement response;
            response.pushPayloadData(getStatus());
            maybeTriggerIRQ(Cause::Acknowledge, response);
            schedule(5ms);
            return true;
        } else {
            // Adjust this response for various types of discs and situations.
            QueueElement response;
            response.pushPayloadData(getStatus());
            response.pushPayloadData(0x00);
            response.pushPayloadData(0x20);
            response.pushPayloadData(0x00);
            response.pushPayloadData("PCSX"sv);
            maybeTriggerIRQ(Cause::Complete, response);
            maybeScheduleNextCommand();
            return false;
        }
    }

    // Command 27.
    bool cdlReadS(const QueueElement &command, bool start) {
        bool ret = cdlReadN(command, start);
        m_readingType = ReadingType::Streaming;
        return ret;
    }

    typedef bool (CDRomImpl::*CommandType)(const QueueElement &, bool);

    const CommandType c_commandsHandlers[31]{
#if 0
        &CDRomImpl::cdlSync, &CDRomImpl::cdlNop, &CDRomImpl::cdlSetLoc, &CDRomImpl::cdlPlay, // 0
        &CDRomImpl::cdlForward, &CDRomImpl::cdlBackward, &CDRomImpl::cdlReadN, &CDRomImpl::cdlStandby, // 4
        &CDRomImpl::cdlStop, &CDRomImpl::cdlPause, &CDRomImpl::cdlInit, &CDRomImpl::cdlMute, // 8
        &CDRomImpl::cdlDemute, &CDRomImpl::cdlSetFilter, &CDRomImpl::cdlSetMode, &CDRomImpl::cdlGetParam, // 12
        &CDRomImpl::cdlGetLocL, &CDRomImpl::cdlGetLocP, &CDRomImpl::cdlReadT, &CDRomImpl::cdlGetTN, // 16
        &CDRomImpl::cdlGetTD, &CDRomImpl::cdlSeekL, &CDRomImpl::cdlSeekP, &CDRomImpl::cdlSetClock,  // 20
        &CDRomImpl::cdlGetClock, &CDRomImpl::cdlTest, &CDRomImpl::cdlID, &CDRomImpl::cdlReadS, // 24
        &CDRomImpl::cdlReset, &CDRomImpl::cdlGetQ, &CDRomImpl::cdlReadTOC,                    // 28
#else
        &CDRomImpl::cdlSync,
        &CDRomImpl::cdlNop,
        &CDRomImpl::cdlSetLoc,
        &CDRomImpl::cdlPlay,  // 0
        &CDRomImpl::cdlForward,
        &CDRomImpl::cdlBackward,
        &CDRomImpl::cdlReadN,
        &CDRomImpl::cdlStandby,  // 4
        &CDRomImpl::cdlStop,
        &CDRomImpl::cdlPause,
        &CDRomImpl::cdlInit,
        &CDRomImpl::cdlMute,  // 8
        &CDRomImpl::cdlDemute,
        &CDRomImpl::cdlSetFilter,
        &CDRomImpl::cdlSetMode,
        &CDRomImpl::cdlGetParam,  // 12
        &CDRomImpl::cdlGetLocL,
        &CDRomImpl::cdlGetLocP,
        &CDRomImpl::cdlSetSession,
        &CDRomImpl::cdlGetTN,  // 16
        &CDRomImpl::cdlGetTD,
        &CDRomImpl::cdlSeekL,
        &CDRomImpl::cdlSeekP,
        nullptr,  // 20
        nullptr,
        &CDRomImpl::cdlTest,
        &CDRomImpl::cdlID,
        &CDRomImpl::cdlReadS,  // 24
        &CDRomImpl::cdlReset,
        &CDRomImpl::cdlGetQ,
        &CDRomImpl::cdlReadTOC,  // 28
#endif
    };

    static constexpr int c_commandsArgumentsCount[31] = {
        0, 0,  3, -1,  // 0
        0, 0,  0, 0,   // 4
        0, 0,  0, 0,   // 8
        0, 2,  1, 0,   // 12
        0, 0,  1, 0,   // 16
        1, 0,  0, 0,   // 20
        0, -1, 0, 0,   // 24
        0, 2,  0,      // 28
    };

    void logCDROM(const QueueElement &command) {
        auto &regs = PCSX::g_emulator->m_cpu->m_regs;

        switch (command.value) {
            case CdlTest:
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlTest %02x\n", regs.pc,
                                    regs.cycle, command.payload[0]);
                break;
            case CdlSetLoc:
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlSetloc %02x:%02x:%02x\n",
                                    regs.pc, regs.cycle, command.payload[0], command.payload[1], command.payload[2]);
                break;
            case CdlPlay:
                if (command.payloadSize == 0) {
                    PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlPlay\n", regs.pc,
                                        regs.cycle);
                } else {
                    PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlPlay %i\n", regs.pc,
                                        regs.cycle, command.payload[0]);
                }
                break;
            case CdlSetFilter:
                PCSX::g_system->log(PCSX::LogClass::CDROM,
                                    "CD-Rom: %08x.%08x] Command: CdlSetfilter file: %i, channel: %i\n", regs.pc,
                                    regs.cycle, command.payload[0], command.payload[1]);
                break;
            case CdlSetMode: {
                auto mode = command.payload[0];
                std::string modeDecode = mode & 1 ? "CDDA" : "DATA";
                if (mode & 2) modeDecode += " Autopause";
                if (mode & 4) modeDecode += " Report";
                if (mode & 8) modeDecode += " SubheaderFilter";
                switch ((mode & 0x30) >> 4) {
                    case 0:
                        modeDecode += " 2048bytes";
                        break;
                    case 1:
                        modeDecode += " 2328bytes";
                        break;
                    case 2:
                        modeDecode += " 2340bytes";
                        break;
                    case 3:
                        modeDecode += " *wrong read mode*";
                        break;
                }
                if (mode & 0x40) modeDecode += " RealTimePlay";
                modeDecode += mode & 0x80 ? " @2x" : " @1x";
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlSetmode %02x (%s)\n",
                                    regs.pc, regs.cycle, command.payload[0], modeDecode);
            } break;
            case CdlGetTN:
                PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlGetTN (returns %i)\n",
                                    regs.pc, regs.cycle, m_iso->getTN());
                break;
            case CdlGetTD: {
                auto ret = m_iso->getTD(command.payload[0]);
                PCSX::g_system->log(PCSX::LogClass::CDROM,
                                    "CD-Rom: %08x.%08x] Command: CdlGetTD %i (returns %02i:%02i:%02i)\n", regs.pc,
                                    regs.cycle, command.payload[0], ret.m, ret.s, ret.f);
            } break;
            default:
                if (command.value >= c_cdCmdEnumCount) {
                    PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: CdlUnknown(0x%02X)\n",
                                        regs.pc, regs.cycle, command.value);
                } else {
                    PCSX::g_system->log(PCSX::LogClass::CDROM, "CD-Rom: %08x.%08x] Command: %s\n", regs.pc, regs.cycle,
                                        commandName(command.value));
                }
                break;
        }
    }
};

}  // namespace

PCSX::CDRom *PCSX::CDRom::factory() { return new CDRomImpl; }

void PCSX::CDRom::parseIso() {
    History::CD::Scope historyScope(History::CD::Operation::IsoScan);
    m_cdromId.clear();
    m_cdromLabel.clear();
    if (m_iso) g_emulator->m_cdromLogger->setDiscSectors(m_iso->getTD(0).toLBA());
    ISO9660Reader reader(m_iso);
    if (reader.failed()) return;
    IO<File> systemcnf(reader.open("SYSTEM.CNF;1"));
    std::string exename;
    m_cdromLabel = StringsHelpers::trim(reader.getLabel());

    if (!systemcnf->failed()) {
        while (!systemcnf->eof()) {
            std::string lineStorage = systemcnf->gets();
            auto line = StringsHelpers::trim(lineStorage);
            if (!StringsHelpers::startsWith(line, "BOOT")) continue;
            auto pathLoc = line.find("cdrom:");
            if (pathLoc == std::string::npos) break;
            auto paths = StringsHelpers::split(line.substr(pathLoc + 6), "/\\");
            if (paths.empty()) break;

            for (auto &path : paths) {
                exename += path;
                exename += '/';
            }
            exename.resize(exename.size() - 1);

            auto filename = paths[paths.size() - 1];
            // pattern is XXXX_YYY.ZZ;1
            if ((filename.size() == 13) && (filename[4] == '_') && (filename[8] == '.') && (filename[11] == ';') &&
                (filename[12] == '1')) {
                m_cdromId = filename.substr(0, 4);
                m_cdromId += filename.substr(5, 3);
                m_cdromId += filename.substr(9, 2);
            }

            break;
        }
    } else {
        IO<File> psxexe(reader.open("PSX.EXE;1"));
        if (!psxexe->failed()) {
            m_cdromId = "SLUS99999";
            exename = "PSX.EXE;1";
        }
    }

    g_system->printf("CD-ROM Label: %.32s\n", m_cdromLabel);
    g_system->printf("CD-ROM ID: %.9s\n", m_cdromId);
    g_system->printf("CD-ROM EXE Name: %.255s\n", exename);
}

bool PCSX::CDRom::isLidOpen() {
    History::CD::Scope historyScope(History::CD::Operation::Lid);
    if (m_lidCloseScheduled) {
        const uint32_t cycle = g_emulator->m_cpu->m_regs.cycle;
        if (((int32_t)(m_lidCloseAtCycles - cycle)) <= 0) {
            m_lidCloseScheduled = false;
            m_lidOpen = false;
        }
    }
    historyScope.result(m_lidOpen);
    History::CD::predicate(History::CD::Predicate::LidState, {uint32_t(m_lidOpen), uint32_t(m_lidCloseScheduled), m_lidCloseAtCycles});
    return m_lidOpen;
}

bool PCSX::CDRom::historyInitialized() const { return m_historyNativeInitialized; }
void PCSX::CDRom::historyReset() { m_historyQueueIds.fill(0); m_historyReadingType = uint8_t(m_readingType); }
std::array<uint8_t, 32> PCSX::CDRom::historyQueue(const QueueElement& queue, uint64_t id) const {
    std::array<uint8_t, 32> out{};
    if (queue.payloadSize > 16 || queue.payloadIndex > 16) { History::fail(History::Failure::Bounds); return out; }
    out[0] = uint8_t(queue.hasValue) | (uint8_t(queue.valueRead) << 1) | (uint8_t(queue.hitMax) << 2) | 8 | (id ? 0 : 16);
    out[1] = queue.value; out[2] = queue.payloadSize; out[3] = queue.payloadIndex;
    for (unsigned b = 0; b < 8; ++b) out[8+b] = id >> (b*8);
    std::copy_n(queue.payload, queue.payloadSize, out.begin()+16);
    return out;
}
void PCSX::CDRom::historyQueueChanged(uint32_t index) {
    if (History::CD::active() && index < 4) m_historyQueueIds[index] = History::CD::nextIdentity();
}
std::array<uint8_t, 384> PCSX::CDRom::historyContext() {
    std::array<uint8_t, 384> out{};
    if (!History::CD::active()) return out;
    const auto put = [&](uint32_t offset, uint64_t value, uint32_t length) {
        for (uint32_t b = 0; b < length; ++b) out[offset+b] = value >> (b*8);
    };
    if (m_historyReadingType != uint8_t(m_readingType)) {
        m_historyReadingType = uint8_t(m_readingType); History::CD::stream(m_historyReadingType);
    }
    const std::array<bool, 19> booleans{m_lidOpen, m_wasLidOpened, m_lidCloseScheduled, m_motorOn,
        m_speedChanged, m_invalidLocL, m_dataRequested, m_subheaderFilter, m_realtime, m_startPlaying,
        m_autoPause, m_report, m_setLocPending, m_muted, m_peakFlag, m_adpcmMuted, m_xaEnded,
        m_xaFirstSector, m_soundMapEnabled};
    uint32_t flags = 0; for (unsigned n = 0; n < booleans.size(); ++n) flags |= uint32_t(booleans[n]) << n;
    put(0, 3, 4); put(4, out.size(), 4); put(8, 1, 4);
    put(16, History::CD::epoch(), 8); put(24, History::CD::audioEpoch(), 8);
    put(32, flags, 4); put(36, m_dataFIFOIndex, 4); put(40, m_dataFIFOSize, 4); put(44, m_dataFIFOPending, 4);
    put(48, m_playStartCycle, 4); put(52, m_lidCloseAtCycles, 4); put(56, m_seed, 8);
    const std::array<uint8_t, 11> registers{m_registerAddress, uint8_t(m_readingState), uint8_t(m_readingType),
        uint8_t(m_status), uint8_t(m_speed), uint8_t(m_readSpan), m_interruptCauseMask, m_playTrack, m_mode,
        m_filterFile, m_filterChannel};
    std::copy(registers.begin(), registers.end(), out.begin()+64);
    out[75] = m_currentPosition.m; out[76] = m_currentPosition.s; out[77] = m_currentPosition.f;
    out[78] = m_seekPosition.m; out[79] = m_seekPosition.s; out[80] = m_seekPosition.f;
    std::copy_n(m_atv, 4, out.begin()+81); std::copy_n(m_atvPending, 4, out.begin()+85);
    std::copy_n(m_lastLocP, 8, out.begin()+96); std::copy_n(m_lastLocL, 8, out.begin()+104);
    const std::array<const QueueElement*, 4> queues{&m_commandFifo, &m_commandExecuting, &m_responseFifo[0], &m_responseFifo[1]};
    for (unsigned n = 0; n < queues.size(); ++n) {
        const auto queue = historyQueue(*queues[n], m_historyQueueIds[n]);
        std::copy(queue.begin(), queue.end(), out.begin()+112+n*32);
    }
    const auto ids = History::CD::identities();
    for (unsigned n = 0; n < ids.size(); ++n) put(240+n*8, ids[n], 8);
    const auto& cpu = g_emulator->m_cpu->m_regs;
    put(304, cpu.scheduleMask, 4); put(308, g_emulator->m_mem->getCHCR<3>(), 4);
    put(312, cpu.lowestTarget, 8);
    constexpr std::array<unsigned, 4> slots{2, 3, 4, 10};
    const auto owners = History::CD::scheduleOwners();
    for (unsigned n = 0; n < slots.size(); ++n) { put(320+n*16, cpu.scheduleTargets[slots[n]], 8); put(328+n*16, owners[n], 8); }
    if (m_dataFIFOIndex > m_dataFIFOSize || m_dataFIFOSize > sizeof(m_dataFIFO) || m_dataFIFOPending > sizeof(m_dataFIFO)) History::fail(History::Failure::Bounds);
    return out;
}
void PCSX::CDRom::history(uint32_t phase) {
    if (!History::CD::active()) return;
    const auto context = historyContext();
    History::emit(History::Kind::CdContext, phase, {3, uint32_t(context.size())}, 0, context.data(), context.size(), 0);
    History::CD::dataBoundary(phase, m_dataFIFO, sizeof(m_dataFIFO));
    History::emit(History::Kind::CdBuffer, phase, {1, m_dataFIFOIndex, m_dataFIFOSize, m_dataFIFOPending, sizeof(m_dataFIFO)}, 0, m_dataFIFO, sizeof(m_dataFIFO), 0);
    constexpr std::array<unsigned, 4> slots{2, 3, 4, 10};
    const auto owners = History::CD::scheduleOwners(); const auto& cpu = g_emulator->m_cpu->m_regs;
    for (unsigned n = 0; n < slots.size(); ++n) {
        const auto target = cpu.scheduleTargets[slots[n]];
        History::emit(History::Kind::CdSchedule, phase ? 7 : 6,
            {slots[n], (cpu.scheduleMask >> slots[n]) & 1, uint32_t(target), uint32_t(target >> 32),
             std::bit_cast<uint32_t>(g_emulator->m_cpu->m_scheduleScales[slots[n]])}, target, nullptr, 0, owners[n]);
    }
}
void PCSX::CDRom::historyResponse(const QueueElement& queue, uint32_t branch, bool result, std::array<uint64_t, 2> before) {
    if (!History::CD::active()) return;
    const uint64_t candidate = History::CD::nextIdentity();
    if (branch == 1) m_historyQueueIds[2] = candidate;
    if (branch == 2) m_historyQueueIds[3] = candidate;
    if (branch == 3) { m_historyQueueIds[2] = before[1]; m_historyQueueIds[3] = candidate; }
    std::array<uint8_t, 64> payload{};
    const std::array<uint64_t, 4> ids{before[0], before[1], m_historyQueueIds[2], m_historyQueueIds[3]};
    for (unsigned n = 0; n < ids.size(); ++n) for (unsigned b = 0; b < 8; ++b) payload[n*8+b] = ids[n] >> (b*8);
    const auto snapshot = historyQueue(queue, candidate); std::copy(snapshot.begin(), snapshot.end(), payload.begin()+32);
    History::emit(History::Kind::CdResponse, branch, {uint32_t(result), queue.value, queue.payloadSize},
        History::CD::access(), payload.data(), payload.size(), candidate);
}

void PCSX::CDRom::historyIsoIdentity(uint32_t phase) {
    if (!History::CD::active()) return;
    if (m_cdromId.size() > 9 || m_cdromLabel.size() > 32) { History::fail(History::Failure::Bounds); return; }
    std::array<uint8_t, 64> payload{};
    payload[0] = m_cdromId.size(); payload[1] = m_cdromLabel.size();
    std::copy(m_cdromId.begin(), m_cdromId.end(), payload.begin()+8);
    std::copy(m_cdromLabel.begin(), m_cdromLabel.end(), payload.begin()+24);
    History::emit(History::Kind::CdMedia, 4+phase, {uint32_t(m_cdromId.size()), uint32_t(m_cdromLabel.size())},
        History::CD::access(), payload.data(), payload.size(), History::CD::operation());
}
