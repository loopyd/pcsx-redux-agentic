/***************************************************************************
 *   Copyright (C) 2026 PCSX-Redux authors                                 *
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

#include "spu/externals.h"
#include "spu/interface.h"
#include "spu/registers.h"

#include <stdexcept>

void PCSX::SPU::impl::save(SaveStates::SPU &spu) {
    if (!beginStateAccess()) throw std::runtime_error("SPU state access unavailable");
    try {
        saveStopped(spu);
    } catch (...) {
        endStateAccess();
        throw;
    }
    endStateAccess();
}

void PCSX::SPU::impl::saveStopped(SaveStates::SPU &spu) {
    if (!m_stateAccess || hMainThread.joinable()) throw std::runtime_error("SPU mixer is not stopped");

    // Capture buffer.
    spu.get<SaveStates::CBCDLeft>().copyFrom(reinterpret_cast<uint8_t *>(captureBuffer.CDCapLeft));
    spu.get<SaveStates::CBCDRight>().copyFrom(reinterpret_cast<uint8_t *>(captureBuffer.CDCapRight));
    spu.get<SaveStates::CBCurrIndex>().value = captureBuffer.currIndex;
    spu.get<SaveStates::CBEndIndex>().value = captureBuffer.endIndex;
    spu.get<SaveStates::CBStartIndex>().value = captureBuffer.startIndex;
    spu.get<SaveStates::CBVoiceIndex>().value = capBufVoiceIndex;

    spu.get<SaveStates::SPURam>().copyFrom(reinterpret_cast<uint8_t *>(spuMem));
    spu.get<SaveStates::SPUPorts>().copyFrom(reinterpret_cast<uint8_t *>(regArea));
    auto &xa = spu.get<SaveStates::XAField>();
    if (xapGlobal) {
        xa.get<SaveStates::XAFrequency>().value = xapGlobal->freq;
        xa.get<SaveStates::XANBits>().value = xapGlobal->nbits;
        xa.get<SaveStates::XANSamples>().value = xapGlobal->nsamples;
        xa.get<SaveStates::XAStereo>().value = xapGlobal->stereo;
        auto &left = xa.get<SaveStates::XAADPCMLeft>();
        left.get<SaveStates::ADPCMDecodeY0>().value = xapGlobal->left.y0;
        left.get<SaveStates::ADPCMDecodeY1>().value = xapGlobal->left.y1;
        auto &right = xa.get<SaveStates::XAADPCMRight>();
        right.get<SaveStates::ADPCMDecodeY0>().value = xapGlobal->right.y0;
        right.get<SaveStates::ADPCMDecodeY1>().value = xapGlobal->right.y1;
        xa.get<SaveStates::XAPCM>().copyFrom(reinterpret_cast<uint8_t *>(xapGlobal->pcm));
    }
    spu.get<SaveStates::SPUIrq>().value = spuIrq;
    spu.get<SaveStates::SPUIrqPtr>().value = 0;
    if (irqAddress) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(spuRamBase);
        const uintptr_t irq = reinterpret_cast<uintptr_t>(irqAddress);
        if (irq < base || irq - base >= 0x80000) throw std::runtime_error("Invalid SPU IRQ address");
        spu.get<SaveStates::SPUIrqPtr>().value = irq - base + 1;
    }

    for (unsigned i = 0; i < MAXCHAN; i++) {
        auto &channel = spu.get<SaveStates::Channels>().value[i];
        auto &data = channel.get<SaveStates::Data>();
        data = s_chan[i].data;
        channel.get<SaveStates::ADSRInfo>() = s_chan[i].adsr.legacy();
        channel.get<SaveStates::ADSRInfoEx>() = s_chan[i].adsr.ex();
        // Each per-voice helper owns the conversion between its runtime state and
        // its savestate-mirror fields; we just hand it the fields.
        s_chan[i].adpcm.saveTo(data.get<Chan::s_1>(), data.get<Chan::s_2>(), data.get<Chan::StartPtr>(),
                               data.get<Chan::CurrPtr>(), data.get<Chan::LoopPtr>(), spuRamBase,
                               data.get<Chan::SB>().value.data(), data.get<Chan::SBPos>());
        s_chan[i].volume.saveTo(data.get<Chan::LeftVolume>(), data.get<Chan::RightVolume>(),
                                data.get<Chan::LeftVolRaw>(), data.get<Chan::RightVolRaw>());
        s_chan[i].interp.saveTo(data.get<Chan::SB>().value.data(), data.get<Chan::spos>(), data.get<Chan::sinc>());
    }

    spu.get<SaveStates::SPUAddr>().value = spuAddr;
    spu.get<SaveStates::SPUCtrl>().value = spuCtrl;
    spu.get<SaveStates::SPUStat>().value = spuStat;
    spu.get<SaveStates::SPUEndx>().value = spuEndx;

    m_noise.saveTo(spu.get<SaveStates::SPUNoiseClock>(), spu.get<SaveStates::SPUNoiseCount>(),
                   spu.get<SaveStates::SPUNoiseVal>());

}

void PCSX::SPU::impl::load(const SaveStates::SPU &spu) {
    if (!beginStateAccess()) throw std::runtime_error("SPU state access unavailable");
    try {
        loadStopped(spu);
    } catch (...) {
        endStateAccess();
        throw;
    }
    endStateAccess();
}

void PCSX::SPU::impl::loadStopped(const SaveStates::SPU &spu) {
    if (!m_stateAccess || hMainThread.joinable()) throw std::runtime_error("SPU mixer is not stopped");

    spu.get<SaveStates::CBCDLeft>().copyTo(reinterpret_cast<uint8_t *>(captureBuffer.CDCapLeft));
    spu.get<SaveStates::CBCDRight>().copyTo(reinterpret_cast<uint8_t *>(captureBuffer.CDCapRight));
    captureBuffer.currIndex = spu.get<SaveStates::CBCurrIndex>().value;
    captureBuffer.endIndex = spu.get<SaveStates::CBEndIndex>().value;
    captureBuffer.startIndex = spu.get<SaveStates::CBStartIndex>().value;
    capBufVoiceIndex = spu.get<SaveStates::CBVoiceIndex>().value;

    spu.get<SaveStates::SPURam>().copyTo(reinterpret_cast<uint8_t *>(spuMem));
    spu.get<SaveStates::SPUPorts>().copyTo(reinterpret_cast<uint8_t *>(regArea));

#if 0
// The xa_decode pointer is grabbed here, which is messy. This needs to be fixed up later.
    if (pF->xa.nsamples <= 4032)  // Start XA again.
        playADPCMchannel(&pF->xa);
#endif

    xapGlobal = 0;

    spuIrq = spu.get<SaveStates::SPUIrq>().value;
    const auto &pSpuIrqIn = spu.get<SaveStates::SPUIrqPtr>().value;
    irqAddress = pSpuIrqIn ? pSpuIrqIn - 1 + spuRamBase : nullptr;

    for (unsigned i = 0; i < MAXCHAN; i++) {
        const auto &channel = spu.get<SaveStates::Channels>().value[i];
        const auto &data = channel.get<SaveStates::Data>();
        s_chan[i].data = data;
        s_chan[i].adsr.legacy() = channel.get<SaveStates::ADSRInfo>();
        s_chan[i].adsr.ex() = channel.get<SaveStates::ADSRInfoEx>();
        s_chan[i].adpcm.loadFrom(data.get<Chan::s_1>(), data.get<Chan::s_2>(), data.get<Chan::StartPtr>(),
                                 data.get<Chan::CurrPtr>(), data.get<Chan::LoopPtr>(), spuRamBase,
                                 data.get<Chan::SB>().value.data(), data.get<Chan::SBPos>());
        s_chan[i].volume.loadFrom(data.get<Chan::LeftVolume>(), data.get<Chan::RightVolume>(),
                                  data.get<Chan::LeftVolRaw>(), data.get<Chan::RightVolRaw>());
        s_chan[i].interp.loadFrom(data.get<Chan::SB>().value.data(), data.get<Chan::spos>(), data.get<Chan::sinc>());
        s_chan[i].data.get<Chan::Mute>().value = false;
        s_chan[i].data.get<Chan::Solo>().value = false;
        s_chan[i].data.get<Chan::IrqDone>().value = 0;
    }

    spuAddr = spu.get<SaveStates::SPUAddr>().value;
    spuCtrl = spu.get<SaveStates::SPUCtrl>().value;
    spuStat = spu.get<SaveStates::SPUStat>().value;
    spuEndx = spu.get<SaveStates::SPUEndx>().value & 0xffffff;

    m_noise.loadFrom(spu.get<SaveStates::SPUNoiseClock>(), spu.get<SaveStates::SPUNoiseCount>(),
                     spu.get<SaveStates::SPUNoiseVal>());

    // Repair some globals. The reverb unit is reset first so no filter history or L/R parity from
    // before the load survives; the register replay below rebuilds its configuration.
    m_reverb.reset();
    for (unsigned i = 0; i <= 62; i += 2) writeRegister(H_Reverb + i, regArea[(H_Reverb + i - 0xc00) >> 1]);
    writeRegister(H_SPUReverbAddr, regArea[(H_SPUReverbAddr - 0xc00) >> 1]);
    writeRegister(H_SPUrvolL, regArea[(H_SPUrvolL - 0xc00) >> 1]);
    writeRegister(H_SPUrvolR, regArea[(H_SPUrvolR - 0xc00) >> 1]);

    writeRegister(H_SPUctrl, (uint16_t)(regArea[(H_SPUctrl - 0xc00) >> 1] | 0x4000));
    writeRegister(H_SPUstat, regArea[(H_SPUstat - 0xc00) >> 1]);
    writeRegister(H_CDLeft, regArea[(H_CDLeft - 0xc00) >> 1]);
    writeRegister(H_CDRight, regArea[(H_CDRight - 0xc00) >> 1]);

    // Fix to prevent new interpolations from crashing.
    for (unsigned i = 0; i < MAXCHAN; i++) s_chan[i].interp.resetAfterLoad();

    // Repair LDChen's ADSR changes.
    if (spuAddr < 0x7ffff) {
        for (unsigned i = 0; i < 24; i++) {
            writeRegister(0x1f801c00 + (i << 4) + 0xc8, regArea[(i << 3) + 0x64]);
            writeRegister(0x1f801c00 + (i << 4) + 0xca, regArea[(i << 3) + 0x65]);
        }
    }


}

bool PCSX::SPU::impl::acceptsPreparedState(const SaveStates::SPU& spu) const noexcept {
    using namespace SaveStates;
    const auto in = [](int64_t v, int64_t lo, int64_t hi) { return v >= lo && v <= hi; };
    const auto numeric = [](int32_t v) { return v >= 0 && v <= 0x7fff0 && !(v & 15); };
    if (spu.get<Channels>().count != 24 || !in(spu.get<CBStartIndex>().value, 0, 16383) ||
        !in(spu.get<CBEndIndex>().value, 0, 16383) || !in(spu.get<CBCurrIndex>().value, 0, 511) ||
        !in(spu.get<CBVoiceIndex>().value, 0, 511) || spu.get<SPUIrq>().value > 65535 ||
        spu.get<SPUNoiseClock>().value > 63 || spu.get<SPUEndx>().value > 0xffffff) return false;
    const auto irq = spu.get<SPUIrqPtr>().value;
    if (irq && (irq > 0x7fff9 || ((irq - 1) & 7))) return false;
    const auto address = spu.get<SPUAddr>().value;
    if (address != UINT32_MAX && (address > 0x7fffe || (address & 1))) return false;
    const auto& xa = spu.get<XAField>();
    if (!in(xa.get<XAFrequency>().value, 0, 44100) ||
        !in(xa.get<XANSamples>().value, 0, xa.get<XAStereo>().value ? 8192 : 16384) ||
        (xa.get<XANBits>().value != 0 && xa.get<XANBits>().value != 4 && xa.get<XANBits>().value != 8 && xa.get<XANBits>().value != 16)) return false;
    for (const auto& channel : spu.get<Channels>().value) {
        const auto& data = channel.get<Data>();
        const auto& adsr = channel.get<ADSRInfoEx>();
        const auto start = data.get<Chan::StartPtr>().value;
        const auto current = data.get<Chan::CurrPtr>().value;
        const auto loop = data.get<Chan::LoopPtr>().value;
        const bool isNew = data.get<Chan::New>().value;
        const bool on = data.get<Chan::On>().value;
        const auto step = data.get<Chan::sinc>().value;
        const auto position = data.get<Chan::spos>().value;
        if ((!numeric(start) && start != AdpcmDecoder::kStoppedOffset) ||
            (!numeric(loop) && loop != AdpcmDecoder::kNullOffset) ||
            (!numeric(current) && current != AdpcmDecoder::kStoppedOffset &&
             !(current == AdpcmDecoder::kNullOffset &&
               (isNew || (!on && step == 0 && position < 65536)))) ||
            data.get<Chan::SB>().count != 64 || !in(data.get<Chan::SBPos>().value, 0, 28) ||
            !in(step, 0, 262128) || !in(position, 0, 327663) ||
            !in(data.get<Chan::RawPitch>().value, 0, 16383) ||
            !in(data.get<Chan::ActFreq>().value, 0, 176389) ||
            !in(data.get<Chan::UsedFreq>().value, 0, 176389) || !in(data.get<Chan::FMod>().value, 0, 2) ||
            !in(data.get<Chan::LeftVolRaw>().value, -32768, 32767) ||
            !in(data.get<Chan::RightVolRaw>().value, -32768, 32767) ||
            !in(data.get<Chan::LeftVolume>().value, -16384, 16383) ||
            !in(data.get<Chan::RightVolume>().value, -16384, 16383) ||
            !in(adsr.get<exState>().value, 0, 4) || !in(adsr.get<exAttackModeExp>().value, 0, 1) ||
            !in(adsr.get<exAttackRate>().value, 0, 127) || !in(adsr.get<exDecayRate>().value, 0, 15) ||
            !in(adsr.get<exSustainLevel>().value, 0, 15) || !in(adsr.get<exSustainModeExp>().value, 0, 1) ||
            !in(adsr.get<exSustainIncrease>().value, 0, 1) || !in(adsr.get<exSustainRate>().value, 0, 127) ||
            !in(adsr.get<exReleaseModeExp>().value, 0, 1) || !in(adsr.get<exReleaseRate>().value, 0, 31) ||
            !in(adsr.get<exEnvelopeVol>().value, 0, 32767) || !in(adsr.get<exEnvelopeVolF>().value, 0, 1048575) ||
            !in(adsr.get<exVolume>().value, 0, 1023)) return false;
        const auto& sb = data.get<Chan::SB>().value;
        if (!in(sb[32].value, 0, 2)) return false;
        if (settings.get<Interpolation>() != 1 || (!on && !isNew) || data.get<Chan::Noise>().value ||
            data.get<Chan::FMod>().value == 2 || step != 0 ||
            data.get<Chan::ActFreq>().value != data.get<Chan::UsedFreq>().value) continue;
        if (isNew) {
            if (start == AdpcmDecoder::kStoppedOffset) continue;
            return false;
        }
        if (position >= 65536) {
            if (current == AdpcmDecoder::kStoppedOffset && data.get<Chan::SBPos>().value >= 28) continue;
            return false;
        }
        const int64_t id1 = int64_t(sb[30].value) - sb[29].value;
        const int64_t id2 = int64_t(sb[31].value) - sb[30].value;
        if (sb[32].value == 2 || (sb[32].value == 1 &&
            ((id1 > 0 && id2 < id1) || (id1 <= 0 && id2 > id1)))) return false;
    }
    return true;
}
