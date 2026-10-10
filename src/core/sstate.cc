/***************************************************************************
 *   Copyright (C) 2019 PCSX-Redux authors                                 *
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

#include "core/sstate.h"

#include <algorithm>
#include <iterator>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

#include "core/callstacks.h"
#include "core/cdrom.h"
#include "core/gpu.h"
#include "core/mdec.h"
#include "core/psxcounters.h"
#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/sio.h"
#include "spu/interface.h"

PCSX::SaveStates::SaveState PCSX::SaveStates::constructSaveState() {
    auto* cd = g_emulator->m_cdrom.get();
    return SaveState{
        SaveStateInfo{
            VersionString{},
            Version{},
        },
        Thumbnail{},
        Memory{
            RAM{g_emulator->m_mem->m_wram},
            ROM{g_emulator->m_mem->m_bios},
            EXP1{g_emulator->m_mem->m_exp1},
            HardwareMemory{g_emulator->m_mem->m_hard},
            SRAM{g_emulator->m_mem->m_sram},
            BIUPlusOne{},
            RamMappingPlusOne{},
        },
        Registers{
            GPR{g_emulator->m_cpu->m_regs.GPR.r},
            CP0{g_emulator->m_cpu->m_regs.CP0.r},
            CP2D{g_emulator->m_cpu->m_regs.CP2D.r},
            CP2C{g_emulator->m_cpu->m_regs.CP2C.r},
            PC{g_emulator->m_cpu->m_regs.pc},
            Code{g_emulator->m_cpu->m_regs.code},
            Cycle{g_emulator->m_cpu->m_regs.cycle},
            ScheduleMask{g_emulator->m_cpu->m_regs.scheduleMask},
            ICacheAddr{g_emulator->m_cpu->m_regs.iCacheAddr},
            ICacheCode{g_emulator->m_cpu->m_regs.iCacheCode},
            NextIsDelaySlot{g_emulator->m_cpu->m_nextIsDelaySlot},
            DelaySlotInfo1{
                DelaySlotIndex{g_emulator->m_cpu->m_delayedLoadInfo[0].index},
                DelaySlotValue{g_emulator->m_cpu->m_delayedLoadInfo[0].value},
                DelaySlotMask{g_emulator->m_cpu->m_delayedLoadInfo[0].mask},
                DelaySlotPcValue{g_emulator->m_cpu->m_delayedLoadInfo[0].pcValue},
                DelaySlotActive{g_emulator->m_cpu->m_delayedLoadInfo[0].active},
                DelaySlotPcActive{g_emulator->m_cpu->m_delayedLoadInfo[0].pcActive},
                DelaySlotFromLink{g_emulator->m_cpu->m_delayedLoadInfo[0].fromLink},
            },
            DelaySlotInfo2{
                DelaySlotIndex{g_emulator->m_cpu->m_delayedLoadInfo[1].index},
                DelaySlotValue{g_emulator->m_cpu->m_delayedLoadInfo[1].value},
                DelaySlotMask{g_emulator->m_cpu->m_delayedLoadInfo[1].mask},
                DelaySlotPcValue{g_emulator->m_cpu->m_delayedLoadInfo[1].pcValue},
                DelaySlotActive{g_emulator->m_cpu->m_delayedLoadInfo[1].active},
                DelaySlotPcActive{g_emulator->m_cpu->m_delayedLoadInfo[1].pcActive},
                DelaySlotFromLink{g_emulator->m_cpu->m_delayedLoadInfo[1].fromLink},
            },
            CurrentDelayedLoad{g_emulator->m_cpu->m_currentDelayedLoad},
            ScheduleTargetsField{g_emulator->m_cpu->m_regs.scheduleTargets},
            InISR{g_emulator->m_cpu->m_inISR},
            MulDivReady{g_emulator->m_cpu->m_regs.muldivReady},
            GteReady{g_emulator->m_cpu->m_regs.gteReady},
        },
        GPU{},
        SPU{},
        SIO{
            SIOBuffer{g_emulator->m_sio->m_buffer},
            SIOStatusReg{g_emulator->m_sio->m_regs.status},
            SIOModeReg{g_emulator->m_sio->m_regs.mode},
            SIOCtrlReg{g_emulator->m_sio->m_regs.control},
            SIOBaudReg{g_emulator->m_sio->m_regs.baud},
            SIOBufferMaxIndex{g_emulator->m_sio->m_maxBufferIndex},
            SIOBufferIndex{g_emulator->m_sio->m_bufferIndex},
            SIOPadState{g_emulator->m_sio->m_padState},
            SIOCurrentDevice{g_emulator->m_sio->m_currentDevice},
            SIOMCD1TempBuffer{g_emulator->m_sio->m_memoryCard[0].m_tempBuffer},
            SIOMCD1DirectoryFlag{g_emulator->m_sio->m_memoryCard[0].m_directoryFlag},
            SIOMCD1ChecksumIn{g_emulator->m_sio->m_memoryCard[0].m_checksumIn},
            SIOMCD1ChecksumOut{g_emulator->m_sio->m_memoryCard[0].m_checksumOut},
            SIOMCD1CommandTicks{g_emulator->m_sio->m_memoryCard[0].m_commandTicks},
            SIOMCD1CurrentCommand{g_emulator->m_sio->m_memoryCard[0].m_currentCommand},
            SIOMCD1Sector{g_emulator->m_sio->m_memoryCard[0].m_sector},
            SIOMCD1DataOffset{g_emulator->m_sio->m_memoryCard[0].m_dataOffset},
            SIOMCD2TempBuffer{g_emulator->m_sio->m_memoryCard[1].m_tempBuffer},
            SIOMCD2DirectoryFlag{g_emulator->m_sio->m_memoryCard[1].m_directoryFlag},
            SIOMCD2ChecksumIn{g_emulator->m_sio->m_memoryCard[1].m_checksumIn},
            SIOMCD2ChecksumOut{g_emulator->m_sio->m_memoryCard[1].m_checksumOut},
            SIOMCD2CommandTicks{g_emulator->m_sio->m_memoryCard[1].m_commandTicks},
            SIOMCD2CurrentCommand{g_emulator->m_sio->m_memoryCard[1].m_currentCommand},
            SIOMCD2Sector{g_emulator->m_sio->m_memoryCard[1].m_sector},
            SIOMCD2DataOffset{g_emulator->m_sio->m_memoryCard[1].m_dataOffset},
            SIOMCD1Data{}, SIOMCD2Data{}, SIOMCD1SPDR{}, SIOMCD2SPDR{},
            SIOMCD1DirectoryIndex{}, SIOMCD2DirectoryIndex{}, SIORxFIFOCountPlusOne{}, SIORxFIFO{},
            SIOPocketstationMaskPlusOne{}, SIOInsertedMaskPlusOne{}, SIODataReg{},
        },
        Hardware{},
        Counters{},
        MDEC{},
        PCdrvFilesField{},
        CallStacks{},
        CDRom{
            CDDataFIFO{cd->m_dataFIFO},
            CDDataFIFOIndex{cd->m_dataFIFOIndex},
            CDDataFIFOSize{cd->m_dataFIFOSize},
            CDDataFIFOPending{cd->m_dataFIFOPending},
            CDRegisterAddress{cd->m_registerAddress},
            CDMotorOn{cd->m_motorOn},
            CDSpeedChanged{cd->m_speedChanged},
            CDInvalidLocL{cd->m_invalidLocL},
            CDDataRequested{cd->m_dataRequested},
            CDSubheaderFilter{cd->m_subheaderFilter},
            CDRealtime{cd->m_realtime},
            CDReadingState{reinterpret_cast<uint8_t&>(cd->m_readingState)},
            CDStartPlaying{cd->m_startPlaying},
            CDReadingType{reinterpret_cast<uint8_t&>(cd->m_readingType)},
            CDAutoPause{cd->m_autoPause},
            CDReport{cd->m_report},
            CDSetLocPending{cd->m_setLocPending},
            CDMuted{cd->m_muted},
            CDPeakFlag{cd->m_peakFlag},
            CDPlayTrack{cd->m_playTrack},
            CDPlayStartCycle{cd->m_playStartCycle},
            CDStatus{reinterpret_cast<uint8_t&>(cd->m_status)},
            CDSpeed{reinterpret_cast<uint8_t&>(cd->m_speed)},
            CDReadSpan{reinterpret_cast<uint8_t&>(cd->m_readSpan)},
            CDInterruptCauseMask{cd->m_interruptCauseMask},
            CDATV{cd->m_atv},
            CDATVPending{cd->m_atvPending},
            CDADPCMMuted{cd->m_adpcmMuted},
            CDMode{cd->m_mode},
            CDFilterFile{cd->m_filterFile},
            CDFilterChannel{cd->m_filterChannel},
            CDXAEnded{cd->m_xaEnded},
            CDXAFirstSector{cd->m_xaFirstSector},
            CDSoundMapEnabled{cd->m_soundMapEnabled},
            CDCurrentPosition{CDMSF{CDMSFMinute{cd->m_currentPosition.m}, CDMSFSecond{cd->m_currentPosition.s},
                                     CDMSFFrame{cd->m_currentPosition.f}}},
            CDSeekPosition{CDMSF{CDMSFMinute{cd->m_seekPosition.m}, CDMSFSecond{cd->m_seekPosition.s},
                                  CDMSFFrame{cd->m_seekPosition.f}}},
            CDLastLocP{cd->m_lastLocP},
            CDLastLocL{cd->m_lastLocL},
            CDSeed{cd->m_seed},
            CDLidOpen{cd->m_lidOpen},
            CDWasLidOpened{cd->m_wasLidOpened},
            CDLidCloseScheduled{cd->m_lidCloseScheduled},
            CDLidCloseAtCycles{cd->m_lidCloseAtCycles},
            CDCommandFifo{CDQueueElement{
                    CDQueuePayload{cd->m_commandFifo.payload},
                    CDQueueValue{cd->m_commandFifo.value},
                    CDQueueValueRead{cd->m_commandFifo.valueRead},
                    CDQueueHasValue{cd->m_commandFifo.hasValue},
                    CDQueueHitMax{cd->m_commandFifo.hitMax},
                    CDQueuePayloadSize{cd->m_commandFifo.payloadSize},
                    CDQueuePayloadIndex{cd->m_commandFifo.payloadIndex},
                }},
            CDCommandExecuting{CDQueueElement{
                    CDQueuePayload{cd->m_commandExecuting.payload},
                    CDQueueValue{cd->m_commandExecuting.value},
                    CDQueueValueRead{cd->m_commandExecuting.valueRead},
                    CDQueueHasValue{cd->m_commandExecuting.hasValue},
                    CDQueueHitMax{cd->m_commandExecuting.hitMax},
                    CDQueuePayloadSize{cd->m_commandExecuting.payloadSize},
                    CDQueuePayloadIndex{cd->m_commandExecuting.payloadIndex},
                }},
            CDResponseFifo0{CDQueueElement{
                    CDQueuePayload{cd->m_responseFifo[0].payload},
                    CDQueueValue{cd->m_responseFifo[0].value},
                    CDQueueValueRead{cd->m_responseFifo[0].valueRead},
                    CDQueueHasValue{cd->m_responseFifo[0].hasValue},
                    CDQueueHitMax{cd->m_responseFifo[0].hitMax},
                    CDQueuePayloadSize{cd->m_responseFifo[0].payloadSize},
                    CDQueuePayloadIndex{cd->m_responseFifo[0].payloadIndex},
                }},
            CDResponseFifo1{CDQueueElement{
                    CDQueuePayload{cd->m_responseFifo[1].payload},
                    CDQueueValue{cd->m_responseFifo[1].value},
                    CDQueueValueRead{cd->m_responseFifo[1].valueRead},
                    CDQueueHasValue{cd->m_responseFifo[1].hasValue},
                    CDQueueHitMax{cd->m_responseFifo[1].hitMax},
                    CDQueuePayloadSize{cd->m_responseFifo[1].payloadSize},
                    CDQueuePayloadIndex{cd->m_responseFifo[1].payloadIndex},
                }},
            CDXALeft{CDADPCM{CDADPCMY0{cd->m_xa.left.y0}, CDADPCMY1{cd->m_xa.left.y1}}},
            CDXARight{CDADPCM{CDADPCMY0{cd->m_xa.right.y0}, CDADPCMY1{cd->m_xa.right.y1}}},
        },
    };
}

namespace PCSX {
struct SaveStateWrapper {
    SaveStateWrapper(SaveStates::SaveState& state_) : state(state_) {}
    SaveStates::SaveState& state;
};
}  // namespace PCSX

namespace PCSX::SaveStates {
namespace {
bool operationBusy = false;
struct OperationLease {
    OperationLease() { operationBusy = true; }
    ~OperationLease() { operationBusy = false; }
};
struct InvalidProducer {};
struct ValidatedRestore {
    ValidatedRestore() : state(constructSaveState()) {}
    SaveState state;
    ::PCSX::SIO::PreparedRestore sio;
    ::PCSX::Memory::RamState ram;
    std::shared_ptr<::PCSX::CDRIso> iso;
    std::unique_ptr<::PCSX::R3000Acpu::PreparedPCdrv> files;
    std::unique_ptr<::PCSX::CallStacks::PreparedState> callstacks;
};
template <typename Field, typename Message>
bool completePointer(const Message& message, uint64_t size) {
    return message.template get<Field>().decodedSize() == size;
}
template <typename Field, typename Message>
bool completeBuffer(const Message& message, uint64_t size) {
    return message.template get<Field>().decodedSize == size;
}
uint32_t encodePointer(const void* pointer, const void* base, size_t bytes, size_t alignment) {
    if (!pointer) return 0;
    const uintptr_t start = reinterpret_cast<uintptr_t>(base);
    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    if (value < start || value - start > bytes || ((value - start) % alignment)) throw InvalidProducer();
    return static_cast<uint32_t>(value - start + 1);
}
enum class SaveMode { Checked, Legacy };
Result contextAvailable() noexcept {
    if (History::stateBoundary() == History::StateBoundary::Unavailable) return Result::UnavailableContext;
    if (operationBusy) return Result::Busy;
    if (!g_system || !g_emulator || !g_emulator->m_cpu || !g_emulator->m_mem || !g_emulator->m_gpu ||
        !g_emulator->m_spu || !g_emulator->m_sio || !g_emulator->m_cdrom || !g_emulator->m_counters ||
        !g_emulator->m_mdec || !g_emulator->m_callStacks || !g_system->m_eventBus || !g_emulator->m_mem->m_wram ||
        !g_emulator->m_mem->m_bios || !g_emulator->m_mem->m_exp1 || !g_emulator->m_mem->m_hard ||
        !g_emulator->m_mem->m_sram || !g_emulator->m_spu->stateAccessAvailable()) return Result::UnavailableContext;
    return Result::Ok;
}
Result boundary(bool paused) noexcept {
    try {
        const auto context = contextAvailable();
        if (context != Result::Ok) return context;
        const auto history = History::stateBoundary();
        if (history == History::StateBoundary::Unavailable) return Result::UnavailableContext;
        if (g_system->quitting()) return Result::Quitting;
        if (paused && g_system->running()) return Result::Running;
        if (history == History::StateBoundary::Recording || g_emulator->m_spu->captureIdentity()[0] == 1)
            return Result::RecorderActive;
        if (history == History::StateBoundary::Unsettled || operationBusy || g_system->inExecute() ||
            g_system->hasPendingSaveStateLoad() || g_system->m_eventBus->dispatching() || !g_emulator->m_gpu->isIdle())
            return Result::Busy;
        ::PCSX::Memory::RamState ram;
        return g_emulator->m_mem->captureRamState(ram) ? Result::Ok : Result::InvalidState;
    } catch (...) {
        return Result::UnavailableContext;
    }
}
void pauseAfterFailure() noexcept {
    try { g_system->pause(); } catch (...) {}
}
Result validateRestore(ValidatedRestore& prepared) {
    const auto& state = prepared.state;
    const auto& info = state.get<SaveStateInfoField>();
    if (info.get<Version>().value != Format::Version || info.get<VersionString>().value != Format::VersionString)
        return Result::UnsupportedVersion;
    const auto& memory = state.get<MemoryField>();
    const auto& regs = state.get<RegistersField>();
    const auto& gpu = state.get<GPUField>();
    const auto& spu = state.get<SPUField>();
    const auto& sio = state.get<SIOField>();
    const auto& cd = state.get<CDRomField>();
    const auto& mdec = state.get<MDECField>();
    if (!completePointer<RAM>(memory, 0x800000) || !completePointer<ROM>(memory, 0x80000) ||
        !completePointer<EXP1>(memory, 0x800000) || !completePointer<HardwareMemory>(memory, 0x10000) ||
        !completePointer<SRAM>(memory, 0x200000) || !completePointer<ICacheAddr>(regs, 0x1000) ||
        !completePointer<ICacheCode>(regs, 0x1000) || !completeBuffer<GPUControl>(gpu, 0x400) ||
        !completeBuffer<GPUVRam>(gpu, 0x100000) || !completeBuffer<SPURam>(spu, 0x80000) ||
        !completeBuffer<SPUPorts>(spu, 0x200) || !completeBuffer<XAPCM>(spu.get<XAField>(), 32768) ||
        !completeBuffer<CBCDLeft>(spu, 32768) || !completeBuffer<CBCDRight>(spu, 32768) ||
        !completePointer<SIOBuffer>(sio, 0x1010) || !completePointer<SIOMCD1TempBuffer>(sio, 128) ||
        !completePointer<SIOMCD2TempBuffer>(sio, 128) || !completePointer<CDDataFIFO>(cd, 2352) ||
        !completePointer<CDATV>(cd, 4) || !completePointer<CDATVPending>(cd, 4) ||
        !completePointer<CDLastLocP>(cd, 8) || !completePointer<CDLastLocL>(cd, 8) ||
        !completeBuffer<MDECBlockBuffer>(mdec, 768) || regs.get<GPR>().count != 34 ||
        regs.get<CP0>().count != 32 || regs.get<CP2D>().count != 32 || regs.get<CP2C>().count != 32 ||
        regs.get<ScheduleTargetsField>().count != 32 || state.get<CountersField>().get<Rcnts>().count != 4 ||
        mdec.get<MDECIQY>().count != 64 || mdec.get<MDECIQUV>().count != 64 ||
        mdec.get<MDECQTY>().count != 64 || mdec.get<MDECQTUV>().count != 64 ||
        mdec.get<MDECScaleTable>().count != 64 || regs.get<CurrentDelayedLoad>().decodedValue() > 1 ||
        regs.get<DelaySlotInfo1>().get<DelaySlotIndex>().decodedValue() >= 32 ||
        regs.get<DelaySlotInfo2>().get<DelaySlotIndex>().decodedValue() >= 32 ||
        (regs.get<ScheduleMask>().decodedValue() & ~0x7ffu)) return Result::InvalidState;
    const auto biu = memory.get<BIUPlusOne>().value;
    const auto mapping = memory.get<RamMappingPlusOne>().value;
    if (biu < 1 || biu > uint64_t(UINT32_MAX) + 1 || mapping < 1 || mapping > 4) return Result::InvalidState;
    prepared.ram = {static_cast<uint32_t>(biu - 1), bool((mapping - 1) & 1), bool((mapping - 1) & 2)};
    if (prepared.ram.setting8MB != bool(g_emulator->settings.get<Emulator::Setting8MB>()))
        return Result::TopologyMismatch;
    const auto queueValid = [](const auto& queue) {
        return completePointer<CDQueuePayload>(queue, 16) &&
               queue.template get<CDQueuePayloadSize>().decodedValue() <= 16 &&
               queue.template get<CDQueuePayloadIndex>().decodedValue() <= 16;
    };
    const auto msfValid = [](const auto& field) {
        return field.template get<CDMSFSecond>().decodedValue() < 60 &&
               field.template get<CDMSFFrame>().decodedValue() < 75;
    };
    if (!queueValid(cd.get<CDCommandFifo>()) || !queueValid(cd.get<CDCommandExecuting>()) ||
        !queueValid(cd.get<CDResponseFifo0>()) || !queueValid(cd.get<CDResponseFifo1>()) ||
        !msfValid(cd.get<CDCurrentPosition>()) || !msfValid(cd.get<CDSeekPosition>()) ||
        cd.get<CDRegisterAddress>().decodedValue() > 3 || cd.get<CDReadingState>().decodedValue() > 2 ||
        cd.get<CDReadingType>().decodedValue() > 3 || cd.get<CDStatus>().decodedValue() > 3 ||
        cd.get<CDSpeed>().decodedValue() > 1 || cd.get<CDReadSpan>().decodedValue() > 2 ||
        cd.get<CDDataFIFOIndex>().decodedValue() > cd.get<CDDataFIFOSize>().decodedValue() ||
        cd.get<CDDataFIFOSize>().decodedValue() > 2352 || cd.get<CDDataFIFOPending>().decodedValue() > 2352)
        return Result::InvalidState;
    const CDADPCM* const histories[] = {&cd.get<CDXALeft>(), &cd.get<CDXARight>()};
    for (const auto* history : histories) {
        const auto y0 = history->get<CDADPCMY0>().decodedValue();
        const auto y1 = history->get<CDADPCMY1>().decodedValue();
        if (y0 < -32768 || y0 > 32767 || y1 < -32768 || y1 > 32767) return Result::InvalidState;
    }
    prepared.iso = g_emulator->m_cdrom->getIso();
    if (!g_emulator->m_spu->acceptsPreparedState(spu) ||
        !g_emulator->m_cdrom->acceptsPreparedState(cd, regs.get<ScheduleMask>().decodedValue()))
        return Result::InvalidState;
    const auto pointerValid = [](uint32_t value) {
        return value == 0 || (value <= 0x800001 && !((value - 1) & 1));
    };
    const uint32_t rl = mdec.get<MDECRl>().value;
    const uint32_t end = mdec.get<MDECRlEnd>().value;
    const uint32_t pos = mdec.get<MDECBlockBufferPos>().value;
    const uint32_t blockSize = mdec.get<MDECReg0>().value & 0x08000000 ? 512 : 768;
    if (!pointerValid(rl) || !pointerValid(end) || pos > blockSize + 1 || mdec.get<MDECCustomScale>().value > 1)
        return Result::InvalidState;
    const bool outputScheduled = regs.get<ScheduleMask>().decodedValue() &
                                 (1u << static_cast<unsigned>(Schedule::MDECOUTDMA));
    const bool decoding = mdec.get<MDECReg1>().value & 0x20000000u;
    if ((rl && end && rl > end) ||
        ((decoding || outputScheduled) && (!rl || !end)) ||
        (decoding && rl == end && !outputScheduled) ||
        (mdec.get<MDECDMAADR>().value && mdec.get<MDECDMACHCR>().value != 0x01000200u))
        return Result::InvalidState;
    for (unsigned i = 0; i < 64; ++i) {
        if (mdec.get<MDECQTY>().value[i].value < 0 || mdec.get<MDECQTY>().value[i].value > 255 ||
            mdec.get<MDECQTUV>().value[i].value < 0 || mdec.get<MDECQTUV>().value[i].value > 255 ||
            mdec.get<MDECScaleTable>().value[i].value < -32768 ||
            mdec.get<MDECScaleTable>().value[i].value > 32767) return Result::InvalidState;
    }
    if (mdec.get<MDECDMAADR>().value && mdec.get<MDECDMACHCR>().value == 0x01000200) {
        const auto bcr = mdec.get<MDECDMABCR>().value;
        const uint64_t bytes = uint64_t(bcr >> 16) * (bcr & 0xffff) * 4;
        if (bytes > uint64_t(INT32_MAX) / 2 || (pos && bytes < blockSize - (pos - 1)) ||
            !g_emulator->m_mem->acceptsStateDma(mdec.get<MDECDMAADR>().value, bytes, prepared.ram,
                                              memory.get<HardwareMemory>().decodedData())) return Result::InvalidState;
    }
    for (const auto& counter : state.get<CountersField>().get<Rcnts>().value) {
        if (counter.get<RcntRate>().value == 0 || counter.get<RcntCounterState>().value > 1 ||
            counter.get<RcntIRQState>().value > 1 || counter.get<RcntGateStarted>().value > 1)
            return Result::InvalidState;
    }
    std::array<bool, 65536> seen{};
    for (const auto& file : state.get<PCdrvFilesField>().value) {
        const auto fd = file.get<PCdrvFD>().value;
        if (seen[fd]) return Result::InvalidState;
        seen[fd] = true;
    }
    return g_emulator->m_sio->prepareRestore(sio, prepared.sio);
}
Result loadPrepared(std::string_view input, bool paused) noexcept {
    const auto guard = boundary(paused);
    if (guard != Result::Ok) return guard;
    OperationLease lease;
    if (input.size() > MaxSize) return Result::TooLarge;
    bool decoded = false, stopped = false, committing = false;
    Result failure = Result::Malformed;
    try {
        std::string bytes(input);
        ValidatedRestore prepared;
        auto& state = prepared.state;
        Protobuf::InSlice slice(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        state.deserialize(&slice, 0);
        decoded = true;
        const auto validation = validateRestore(prepared);
        if (validation != Result::Ok) return validation;
        SaveStateWrapper wrapper(state);
        std::vector<::PCSX::R3000Acpu::PCdrvRestore> files;
        files.reserve(state.get<PCdrvFilesField>().value.size());
        for (const auto& file : state.get<PCdrvFilesField>().value)
            files.push_back({file.get<PCdrvFD>().value, file.get<PCdrvFilename>().value, file.get<PCdrvCreate>().value});
        prepared.files = g_emulator->m_cpu->preparePCdrv(files);
        prepared.callstacks = g_emulator->m_callStacks->prepareState(&wrapper);
        if (!g_emulator->m_spu->beginStateAccess()) return Result::UnavailableContext;
        stopped = true;
        committing = true;
        g_emulator->m_spu->invalidateCapture();
        History::fail(History::Failure::Discontinuity);
        g_emulator->m_cpu->Reset();
        state.commit();
        g_emulator->m_sio->restorePrepared(prepared.sio);
        g_emulator->m_mem->restoreRamState(prepared.ram);
        g_emulator->m_cpu->m_regs.lowestTarget = g_emulator->m_cpu->m_regs.cycle;
        g_emulator->m_cpu->m_regs.previousCycles = g_emulator->m_cpu->m_regs.cycle;
        g_emulator->m_cpu->m_regs.pc &= ~3u;
        g_emulator->m_gpu->deserialize(&wrapper);
        g_emulator->m_spu->loadStopped(state.get<SPUField>());
        g_emulator->m_counters->deserialize(&wrapper);
        g_emulator->m_mdec->deserialize(&wrapper);
        if (!g_emulator->m_cpu->commitPCdrv(*prepared.files)) throw std::runtime_error("PCdrv restore failed");
        g_emulator->m_callStacks->commitState(*prepared.callstacks);
        g_emulator->m_spu->endStateAccess();
        stopped = false;
        g_system->m_eventBus->signal(Events::ExecutionFlow::SaveStateLoaded{});
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        failure = committing ? Result::CommitFailed : Result::AllocationFailed;
    } catch (...) {
        failure = committing ? Result::CommitFailed : decoded ? Result::InvalidState : Result::Malformed;
    }
    if (stopped) {
        try { g_emulator->m_spu->endStateAccess(); }
        catch (...) { failure = committing ? Result::CommitFailed : Result::AllocationFailed; }
    }
    if (committing || failure == Result::CommitFailed) pauseAfterFailure();
    return failure;
}
}
Result context() noexcept { return boundary(true); }
Result loadChecked(std::string_view data) noexcept { return loadPrepared(data, true); }
namespace {
CPUObservation projectCPU(const SaveState& state) {
    const auto& regs = state.get<RegistersField>();
    CPUObservation output{};
    output.pc = regs.get<PC>().decodedValue();
    output.code = regs.get<Code>().decodedValue();
    output.cycle = regs.get<Cycle>().decodedValue();
    output.currentDelayedLoad = regs.get<CurrentDelayedLoad>().decodedValue();
    output.inISR = regs.get<InISR>().decodedValue();
    output.nextIsDelaySlot = regs.get<NextIsDelaySlot>().decodedValue();
    std::copy_n(regs.get<GPR>().copy, output.gpr.size(), output.gpr.begin());
    std::copy_n(regs.get<CP0>().copy, output.cp0.size(), output.cp0.begin());
    const auto delay = [](const DelaySlotInfo& info) {
        return CPUObservation::Delay{info.get<DelaySlotIndex>().decodedValue(),
            info.get<DelaySlotValue>().decodedValue(), info.get<DelaySlotMask>().decodedValue(),
            info.get<DelaySlotPcValue>().decodedValue(), info.get<DelaySlotActive>().decodedValue(),
            info.get<DelaySlotPcActive>().decodedValue(), info.get<DelaySlotFromLink>().decodedValue(), info.hasData()};
    };
    output.delays = {delay(regs.get<DelaySlotInfo1>()), delay(regs.get<DelaySlotInfo2>())};
    const auto& memory = state.get<MemoryField>();
    const auto word = [&](int64_t address) -> std::optional<uint32_t> {
        if (address < 0 || address > UINT32_MAX || (address & 3)) return {};
        const auto segment = uint32_t(address) >> 29;
        if (segment != 0 && segment != 4 && segment != 5) return {};
        const uint32_t physical = uint32_t(address) & 0x1fffffffu;
        const uint8_t* bytes = nullptr;
        if (physical < 0x200000) bytes = memory.get<RAM>().decodedData() + physical;
        else if (physical >= 0x1fc00000 && physical < 0x1fc80000)
            bytes = memory.get<ROM>().decodedData() + physical - 0x1fc00000;
        else if (physical >= 0x1f800000 && physical < 0x1f800400)
            bytes = memory.get<HardwareMemory>().decodedData() + physical - 0x1f800000;
        if (!bytes) return {};
        return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
    };
    output.nextWord = word(output.pc);
    for (int offset = -8; offset <= 8; offset += 4) {
        const int64_t address = int64_t(output.pc) + offset;
        if (const auto value = word(address))
            output.nearby[output.nearbyCount++] = {uint32_t(address), *value};
    }
    return output;
}

Result savePrepared(std::string& output, SaveMode mode, CPUObservation* cpu = nullptr,
                    const std::function<Result(const SaveState&)>* operation = nullptr) noexcept {
    output.clear();
    const auto guard = mode == SaveMode::Checked ? boundary(true) : contextAvailable();
    if (guard != Result::Ok) return guard;
    OperationLease lease;
    bool stopped = false;
    Result failure = Result::InvalidState;
    try {
        SaveState state = constructSaveState();
        SaveStateWrapper wrapper(state);
        ::PCSX::Memory::RamState ram;
        if (!g_emulator->m_mem->captureRamState(ram)) return Result::InvalidState;
        state.get<SaveStateInfoField>().get<VersionString>().value = Format::VersionString;
        state.get<SaveStateInfoField>().get<Version>().value = Format::Version;
        state.get<MemoryField>().get<BIUPlusOne>().value = uint64_t(ram.biu) + 1;
        state.get<MemoryField>().get<RamMappingPlusOne>().value = 1 + unsigned(ram.writes) + 2 * unsigned(ram.setting8MB);
        const auto cards = g_emulator->m_sio->saveState(state.get<SIOField>());
        if (cards != Result::Ok) return cards;
        g_emulator->m_gpu->serialize(&wrapper);
        g_emulator->m_counters->serialize(&wrapper);
        g_emulator->m_mdec->serialize(&wrapper);
        g_emulator->m_cpu->listAllPCdrvFiles([&state](uint16_t fd, std::filesystem::path filename, bool create) {
            state.get<PCdrvFilesField>().value.emplace_back(fd, filename.string(), create);
        });
        g_emulator->m_callStacks->serialize(&wrapper);
        auto& spu = state.get<SPUField>();
        spu.get<SPURam>().allocate();
        spu.get<SPUPorts>().allocate();
        spu.get<CBCDLeft>().allocate();
        spu.get<CBCDRight>().allocate();
        spu.get<XAField>().get<XAPCM>().reset();
        if (!g_emulator->m_spu->beginStateAccess()) return Result::UnavailableContext;
        stopped = true;
        g_emulator->m_spu->saveStopped(spu);
        stopped = false;
        try { g_emulator->m_spu->endStateAccess(); }
        catch (...) { pauseAfterFailure(); return Result::AllocationFailed; }
        Protobuf::OutSlice slice(MaxSize);
        state.serialize(&slice);
        auto bytes = slice.finalize();
        if (mode == SaveMode::Checked) {
            ValidatedRestore readback;
            Protobuf::InSlice check(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            readback.state.deserialize(&check, 0);
            const auto validation = validateRestore(readback);
            if (validation != Result::Ok) return validation;
            if (cpu) *cpu = projectCPU(readback.state);
            if (operation) return (*operation)(readback.state);
        }
        if (!cpu) output.swap(bytes);
        return Result::Ok;
    } catch (const Protobuf::OutputTooLarge&) { failure = Result::TooLarge; }
    catch (const std::bad_alloc&) { failure = Result::AllocationFailed; }
    catch (...) { failure = Result::InvalidState; }
    if (stopped) {
        try { g_emulator->m_spu->endStateAccess(); }
        catch (...) { failure = Result::AllocationFailed; pauseAfterFailure(); }
    }
    return failure;
}
}
Result saveChecked(std::string& output) noexcept { return savePrepared(output, SaveMode::Checked); }
Result captureCPUChecked(CPUObservation& output) noexcept {
    output = {};
    std::string bytes;
    return savePrepared(bytes, SaveMode::Checked, &output);
}
Result withCheckedState(const std::function<Result(const SaveState&)>& operation) noexcept {
    if (!operation) return Result::InvalidState;
    std::string bytes;
    return savePrepared(bytes, SaveMode::Checked, nullptr, &operation);
}
}

std::string PCSX::SaveStates::save() {
    std::string output;
    if (savePrepared(output, SaveMode::Legacy) != Result::Ok) return {};
    return output;
}

void PCSX::CallStacks::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& callstacks = w->state.get<SaveStates::CallStacksField>().get<CallStacksMessageField>().value;
    for (auto& callstack : getCallstacks()) {
        SaveStates::CallStack sscallstack{};
        sscallstack.get<LowSP>().value = callstack.getLow();
        sscallstack.get<HighSP>().value = callstack.getHigh();
        sscallstack.get<PresumedRA>().value = callstack.ra;
        sscallstack.get<PresumedFP>().value = callstack.fp;
        sscallstack.get<CallstackIsCurrent>().value = &callstack == m_current;
        for (auto& call : callstack.calls) {
            sscallstack.get<Calls>().value.emplace_back(call.ra, call.sp, call.fp, call.shadow);
        }
        callstacks.emplace_back(sscallstack);
    }
    w->state.get<SaveStates::CallStacksField>().get<CallStacksCurrentSP>().value = m_currentSP;
}

static void setU32(uint8_t* ptr, uint32_t value) {
    ptr[0] = value & 0xff;
    ptr[1] = (value >> 8) & 0xff;
    ptr[2] = (value >> 16) & 0xff;
    ptr[3] = (value >> 24) & 0xff;
}

void PCSX::GPU::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& gpu = w->state.get<GPUField>();
    gpu.get<GPUStatus>() = readStatus();
    auto vram = getVRAM();
    if (vram.size() != 0x100000 || !vram.data<uint8_t>()) throw SaveStates::InvalidProducer();
    gpu.get<GPUVRam>().copyFrom(vram.data<uint8_t>());
    gpu.get<GPUControl>().allocate();
    const auto control = gpu.get<GPUControl>().value;

    for (unsigned i = 0; i < 256; i++) {
        setU32(control + i * 4, m_statusControl[i]);
    }
}

void PCSX::MDEC::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    uint8_t* base = g_emulator->m_mem->m_wram;
    auto& mdecSave = w->state.get<MDECField>();

    mdecSave.get<MDECReg0>().value = mdec.reg0;
    mdecSave.get<MDECReg1>().value = mdec.reg1;
    mdecSave.get<MDECRl>().value = SaveStates::encodePointer(mdec.rl, base, 0x800000, 2);
    mdecSave.get<MDECRlEnd>().value = SaveStates::encodePointer(mdec.rl_end, base, 0x800000, 2);
    mdecSave.get<MDECBlockBufferPos>().value = SaveStates::encodePointer(mdec.block_buffer_pos, mdec.block_buffer,
                                                          sizeof(mdec.block_buffer), 1);
    mdecSave.get<MDECBlockBuffer>().copyFrom(mdec.block_buffer);
    mdecSave.get<MDECDMAADR>().value = mdec.pending_dma1.adr;
    mdecSave.get<MDECDMABCR>().value = mdec.pending_dma1.bcr;
    mdecSave.get<MDECDMACHCR>().value = mdec.pending_dma1.chcr;
    for (unsigned i = 0; i < 64; i++) {
        mdecSave.get<MDECIQY>().value[i].value = iq_y[i];
        mdecSave.get<MDECIQUV>().value[i].value = iq_uv[i];
        mdecSave.get<MDECQTY>().value[i].value = qt_y[i];
        mdecSave.get<MDECQTUV>().value[i].value = qt_uv[i];
        mdecSave.get<MDECScaleTable>().value[i].value = scaletable[i];
    }
    mdecSave.get<MDECCustomScale>().value = customScaleTable ? 1 : 0;
}

void PCSX::Counters::serialize(SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& counters = w->state.get<CountersField>();
    for (unsigned i = 0; i < CounterQuantity; i++) {
        counters.get<Rcnts>().value[i].get<RcntMode>().value = m_rcnts[i].mode;
        counters.get<Rcnts>().value[i].get<RcntTarget>().value = m_rcnts[i].target;
        counters.get<Rcnts>().value[i].get<RcntRate>().value = m_rcnts[i].rate;
        counters.get<Rcnts>().value[i].get<RcntIRQ>().value = m_rcnts[i].irq;
        counters.get<Rcnts>().value[i].get<RcntCounterState>().value = m_rcnts[i].counterState;
        counters.get<Rcnts>().value[i].get<RcntIRQState>().value = m_rcnts[i].irqState;
        counters.get<Rcnts>().value[i].get<RcntCycle>().value = m_rcnts[i].cycle;
        counters.get<Rcnts>().value[i].get<RcntCycleStart>().value = m_rcnts[i].cycleStart;
        counters.get<Rcnts>().value[i].get<RcntGateStarted>().value = m_rcnts[i].gateStarted;
    }
    counters.get<HSyncCount>().value = m_hSyncCount;
    counters.get<PSXNextCounter>().value = m_psxNextCounter;
}

bool PCSX::SaveStates::load(std::string_view data) {
    return loadPrepared(data, false) == Result::Ok;
}

void PCSX::SaveStates::loadSafe(std::string&& data) {
    // Inside Execute(), we're being called from something the CPU reached: a
    // Lua event listener (including a Pause listener fired by a breakpoint), an
    // ImGui menu drawn during vsync, the web server polled on hsync.
    // Counters::update() and branchTest() both cached
    // values before that call and go on using them afterwards, so swapping the
    // state here corrupts whatever those frames touch next. Let the main loop
    // do it once they're gone.
    if (g_system->inExecute()) {
        g_system->scheduleSaveStateLoad(std::move(data));
    } else {
        load(data);
    }
}

std::unique_ptr<PCSX::CallStacks::PreparedState> PCSX::CallStacks::prepareState(const SaveStateWrapper* w) const {
    using namespace SaveStates;
    auto prepared = std::make_unique<PreparedState>();
    const auto& callstacks = w->state.get<CallStacksField>().get<CallStacksMessageField>().value;
    prepared->entries.reserve(callstacks.size());
    for (const auto& saved : callstacks) {
        auto node = std::make_unique<CallStack>();
        node->ra = saved.get<PresumedRA>().value;
        node->fp = saved.get<PresumedFP>().value;
        for (const auto& call : saved.get<Calls>().value) {
            auto next = std::make_unique<CallStack::Call>(call.get<CallSP>().value, call.get<CallFP>().value,
                                                        call.get<CallRA>().value, call.get<Shadow>().value);
            node->calls.push_back(next.release());
        }
        if (saved.get<CallstackIsCurrent>().value) prepared->current = node.get();
        prepared->entries.push_back({saved.get<LowSP>().value, saved.get<HighSP>().value, std::move(node)});
    }
    prepared->currentSP = w->state.get<CallStacksField>().get<CallStacksCurrentSP>().value;
    return prepared;
}

void PCSX::CallStacks::commitState(PreparedState& prepared) noexcept {
    m_callstacks.destroyAll();
    for (auto& entry : prepared.entries) m_callstacks.insert(entry.low, entry.high, entry.node.release());
    m_current = prepared.current;
    m_currentSP = prepared.currentSP;
}

void PCSX::CallStacks::deserialize(const SaveStateWrapper* w) {
    auto prepared = prepareState(w);
    commitState(*prepared);
}

static uint32_t getU32(const uint8_t* ptr) { return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24); }

void PCSX::GPU::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    reset();
    auto& gpu = w->state.get<GPUField>();
    restoreStatus(gpu.get<GPUStatus>().value);
    if (gpu.get<GPUVRam>().value) {
        partialUpdateVRAM(0, 0, 1024, 512, reinterpret_cast<const uint16_t*>(gpu.get<GPUVRam>().value));
    } else {
        clearVRAM();
    }
    const auto control = gpu.get<GPUControl>().value;

    for (unsigned i = 0; i < 256; i++) {
        m_statusControl[i] = getU32(control + i * 4);
    }

    writeStatus(m_statusControl[0]);
    writeStatus(m_statusControl[1]);
    writeStatus(m_statusControl[2]);
    writeStatus(m_statusControl[3]);
    writeStatus(m_statusControl[8]);  // try to repair things
    writeStatus(m_statusControl[6]);
    writeStatus(m_statusControl[7]);
    writeStatus(m_statusControl[5]);
    writeStatus(m_statusControl[4]);
}

void PCSX::MDEC::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    uint8_t* base = g_emulator->m_mem->m_wram;
    auto& mdecSave = w->state.get<MDECField>();

    mdec.reg0 = mdecSave.get<MDECReg0>().value;
    mdec.reg1 = mdecSave.get<MDECReg1>().value;
    const auto rl = mdecSave.get<MDECRl>().value;
    const auto end = mdecSave.get<MDECRlEnd>().value;
    mdec.rl = rl ? reinterpret_cast<uint16_t*>(base + rl - 1) : nullptr;
    mdec.rl_end = end ? reinterpret_cast<uint16_t*>(base + end - 1) : nullptr;
    const auto pos = mdecSave.get<MDECBlockBufferPos>().value;
    mdec.block_buffer_pos = pos ? mdec.block_buffer + pos - 1 : nullptr;
    mdecSave.get<MDECBlockBuffer>().copyTo(mdec.block_buffer);
    mdec.pending_dma1.adr = mdecSave.get<MDECDMAADR>().value;
    mdec.pending_dma1.bcr = mdecSave.get<MDECDMABCR>().value;
    mdec.pending_dma1.chcr = mdecSave.get<MDECDMACHCR>().value;
    for (unsigned i = 0; i < 64; i++) {
        iq_y[i] = mdecSave.get<MDECIQY>().value[i].value;
        iq_uv[i] = mdecSave.get<MDECIQUV>().value[i].value;
    }
    // Fields 12-15 were added without a version bump. A state from before them
    // deserialises zero elements into the repeated fields, which would otherwise
    // leave all-zero quant tables and a zero scale matrix. Those emulators always
    // decoded with the standard matrix; the raw quant tables are recovered from
    // the cached ones, which do carry whatever the game uploaded.
    if (mdecSave.get<MDECQTY>().count == 0 || mdecSave.get<MDECQTUV>().count == 0 ||
        mdecSave.get<MDECScaleTable>().count == 0) {
        standardTables_init();
        qtab_fromIqtab(qt_y, iq_y);
        qtab_fromIqtab(qt_uv, iq_uv);
        return;
    }
    for (unsigned i = 0; i < 64; i++) {
        qt_y[i] = static_cast<uint8_t>(mdecSave.get<MDECQTY>().value[i].value);
        qt_uv[i] = static_cast<uint8_t>(mdecSave.get<MDECQTUV>().value[i].value);
        scaletable[i] = static_cast<int16_t>(mdecSave.get<MDECScaleTable>().value[i].value);
    }
    customScaleTable = mdecSave.get<MDECCustomScale>().value != 0;
    // Not serialised: an all-zero matrix is what a console holds before MDEC(3).
    scaleTableUploaded = std::any_of(std::begin(scaletable), std::end(scaletable), [](int16_t v) { return v != 0; });
    warnedNoScaleTable = false;
}

void PCSX::Counters::deserialize(const SaveStateWrapper* w) {
    using namespace SaveStates;
    auto& counters = w->state.get<CountersField>();
    for (unsigned i = 0; i < CounterQuantity; i++) {
        m_rcnts[i].mode = counters.get<Rcnts>().value[i].get<RcntMode>().value;
        m_rcnts[i].target = counters.get<Rcnts>().value[i].get<RcntTarget>().value;
        m_rcnts[i].rate = counters.get<Rcnts>().value[i].get<RcntRate>().value;
        m_rcnts[i].irq = counters.get<Rcnts>().value[i].get<RcntIRQ>().value;
        m_rcnts[i].counterState = counters.get<Rcnts>().value[i].get<RcntCounterState>().value;
        m_rcnts[i].irqState = counters.get<Rcnts>().value[i].get<RcntIRQState>().value;
        m_rcnts[i].cycle = counters.get<Rcnts>().value[i].get<RcntCycle>().value;
        m_rcnts[i].cycleStart = counters.get<Rcnts>().value[i].get<RcntCycleStart>().value;
        m_rcnts[i].gateStarted = counters.get<Rcnts>().value[i].get<RcntGateStarted>().value;
    }
    m_hSyncCount = counters.get<HSyncCount>().value;
    m_psxNextCounter = counters.get<PSXNextCounter>().value;

    calculateHsync();
    // Recalculate rates from mode registers (handles overclock changes and dotclock)
    m_rcnts[3].target =
        (g_emulator->m_psxClockSpeed / (FrameRate[g_emulator->settings.get<Emulator::SettingVideo>()] *
                                        m_HSyncTotal[g_emulator->settings.get<Emulator::SettingVideo>()]));
    for (unsigned i = 0; i < 3; i++) {
        recalculateRate(i);
    }

    m_audioFrames = g_emulator->m_spu->getCurrentFrames();
}
