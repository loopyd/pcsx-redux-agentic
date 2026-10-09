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

#include "cdrom/cdriso.h"
#include <limits>
#include "core/transactions.h"

#include "supportpsx/iec-60908b.h"

////////////////////////////////////////////////////////////////////////////////
//
// Sector types
//
// Mode 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 01
// 0010h [---DATA...
// ...
// 0800h                                     ...DATA---]
// 0810h [---EDC---] 00 00 00 00 00 00 00 00 [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 1
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0810h             ...DATA---] [---EDC---] [---ECC...
// ...
// 0920h                                      ...ECC---]
// -----------------------------------------------------
//
// Mode 2 (XA), form 2
// -----------------------------------------------------
//        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
// 0000h 00 FF FF FF FF FF FF FF FF FF FF 00 [-ADDR-] 02
// 0010h [--FLAGS--] [--FLAGS--] [---DATA...
// ...
// 0920h                         ...DATA---] [---EDC---]
// -----------------------------------------------------
//
// ADDR:  Sector address, encoded as minutes:seconds:frames in BCD
// FLAGS: Used in Mode 2 (XA) sectors describing the type of sector; repeated
//        twice for redundancy
// DATA:  Area of the sector which contains the actual data itself
// EDC:   Error Detection Code
// ECC:   Error Correction Code
//

PCSX::CDRIso::CDRIso() {
    m_zstr.next_in = Z_NULL;
    m_zstr.avail_in = 0;
    m_zstr.zalloc = Z_NULL;
    m_zstr.zfree = Z_NULL;
    m_zstr.opaque = Z_NULL;
    auto ret = inflateInit2(&m_zstr, -15);
    if (ret != Z_OK) throw("Unable to initialize zlib context");
}

// this function tries to get the .sub file of the given .img
bool PCSX::CDRIso::opensubfile(const char *isoname) {
    char subname[MAXPATHLEN];

    // copy name of the iso and change extension from .img to .sub
    strncpy(subname, isoname, sizeof(subname));
    subname[MAXPATHLEN - 1] = '\0';

    if (strlen(subname) >= 4) {
        strcpy(subname + strlen(subname) - 4, ".sub");
    }

    m_subHandle.setFile(new UvFile(subname));
    if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
        m_subHandle.asA<UvFile>()->startCaching();
    }
    if (!m_subHandle->failed()) return true;
    m_subHandle.reset();

    if (strlen(subname) >= 8) {
        strcpy(subname + strlen(subname) - 8, ".sub");
    }

    m_subHandle.setFile(new UvFile(subname));
    if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
        m_subHandle.asA<UvFile>()->startCaching();
    }
    if (m_subHandle->failed()) {
        m_subHandle.reset();
        return false;
    }

    return true;
}

ssize_t PCSX::CDRIso::cdread_normal(IO<File> f, unsigned int base, void *dest, int sector) {
    return f->readAt(dest, IEC60908b::FRAMESIZE_RAW, base + sector * IEC60908b::FRAMESIZE_RAW);
}

ssize_t PCSX::CDRIso::cdread_sub_mixed(IO<File> f, unsigned int base, void *dest, int sector) {
    int ret;

    ret = f->readAt(dest, IEC60908b::FRAMESIZE_RAW,
                    base + sector * (IEC60908b::FRAMESIZE_RAW + IEC60908b::SUB_FRAMESIZE));
    f->readAt(m_subbuffer.raw, IEC60908b::SUB_FRAMESIZE,
              base + sector * (IEC60908b::FRAMESIZE_RAW + IEC60908b::SUB_FRAMESIZE) + IEC60908b::FRAMESIZE_RAW);

    if (m_subChanRaw) decodeRawSubData();

    return ret;
}

static int uncompress2_internal(void *out, unsigned long *out_size, void *in, unsigned long in_size, z_stream *z) {
    int ret = 0;

    ret = inflateReset(z);
    if (ret != Z_OK) return ret;

    z->next_in = reinterpret_cast<Bytef *>(in);
    z->avail_in = in_size;
    z->next_out = reinterpret_cast<Bytef *>(out);
    z->avail_out = *out_size;

    ret = inflate(z, Z_NO_FLUSH);

    *out_size -= z->avail_out;
    return ret == 1 ? 0 : ret;
}

ssize_t PCSX::CDRIso::cdread_compressed(IO<File> f, unsigned int base, void *dest, int sector) {
    unsigned long cdbuffer_size, cdbuffer_size_expect;
    unsigned int start_byte, size;
    int is_compressed;
    int ret, block;

    if (base) sector += base / 2352;

    block = sector >> m_compr_img->block_shift;
    m_compr_img->sector_in_blk = sector & ((1 << m_compr_img->block_shift) - 1);

    if (block == m_compr_img->current_block) {
        // printf("hit sect %d\n", sector);
        goto finish;
    }

    if (sector >= m_compr_img->index_len * 16) {
        PCSX::g_system->printf("sector %d is past img end\n", sector);
        return -1;
    }

    start_byte = m_compr_img->index_table[block] & 0x7fffffff;
    if (m_cdHandle->rSeek(start_byte, SEEK_SET) != 0) {
        PCSX::g_system->printf("seek error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    is_compressed = !(m_compr_img->index_table[block] & 0x80000000);
    size = (m_compr_img->index_table[block + 1] & 0x7fffffff) - start_byte;
    if (size > sizeof(m_compr_img->buff_compressed)) {
        PCSX::g_system->printf("block %d is too large: %u\n", block, size);
        return -1;
    }

    if (m_cdHandle->read(is_compressed ? m_compr_img->buff_compressed : m_compr_img->buff_raw[0], size) != size) {
        PCSX::g_system->printf("read error for block %d at %x: ", block, start_byte);
        perror(NULL);
        return -1;
    }

    if (is_compressed) {
        cdbuffer_size_expect = sizeof(m_compr_img->buff_raw[0]) << m_compr_img->block_shift;
        cdbuffer_size = cdbuffer_size_expect;
        ret =
            uncompress2_internal(m_compr_img->buff_raw[0], &cdbuffer_size, m_compr_img->buff_compressed, size, &m_zstr);
        if (ret != 0) {
            PCSX::g_system->printf("uncompress failed with %d for block %d, sector %d\n", ret, block, sector);
            return -1;
        }
        if (cdbuffer_size != cdbuffer_size_expect)
            PCSX::g_system->printf("cdbuffer_size: %lu != %lu, sector %d\n", cdbuffer_size, cdbuffer_size_expect,
                                   sector);
    }

    // done at last!
    m_compr_img->current_block = block;

finish:
    if (dest != m_cdbuffer)  // copy avoid HACK
        memcpy(dest, m_compr_img->buff_raw[m_compr_img->sector_in_blk], IEC60908b::FRAMESIZE_RAW);
    return IEC60908b::FRAMESIZE_RAW;
}

ssize_t PCSX::CDRIso::cdread_2048(IO<File> f, unsigned int base, void *dest_, int sector) {
    uint8_t *dest = reinterpret_cast<uint8_t *>(dest_);
    int ret;

    ret = f->readAt(dest + 12 + 4 + 8, 2048, base + sector * 2048);

    dest[0] = 0x00;
    dest[1] = 0xff;
    dest[2] = 0xff;
    dest[3] = 0xff;
    dest[4] = 0xff;
    dest[5] = 0xff;
    dest[6] = 0xff;
    dest[7] = 0xff;
    dest[8] = 0xff;
    dest[9] = 0xff;
    dest[10] = 0xff;
    dest[11] = 0x00;
    IEC60908b::MSF(sector + 150).toBCD(dest + 12);
    dest[15] = 2;
    dest[16] = dest[20] = 0;
    dest[17] = dest[21] = 0;
    dest[18] = dest[22] = 8;
    dest[19] = dest[23] = 0;

    IEC60908b::computeEDCECC(dest);

    return ret;
}

uint8_t *PCSX::CDRIso::getBuffer() {
    if (m_useCompressed) {
        return m_compr_img->buff_raw[m_compr_img->sector_in_blk] + 12;
    } else {
        return m_cdbuffer + 12;
    }
}

void PCSX::CDRIso::printTracks() {
    for (int i = 1; i <= m_numtracks; i++) {
        PCSX::g_system->printf(_("Track %.2d (%s) - Start %.2d:%.2d:%.2d, Length %.2d:%.2d:%.2d\n"), i,
                               (m_ti[i].type == TrackType::DATA        ? "DATA"
                                : m_ti[i].cddatype == trackinfo::CCDDA ? "CZDA"
                                                                       : "CDDA"),
                               m_ti[i].start.m, m_ti[i].start.s, m_ti[i].start.f, m_ti[i].length.m, m_ti[i].length.s,
                               m_ti[i].length.f);
    }
}

bool PCSX::CDRIso::open(IO<File> isoFile) {
    if (History::CD::active()) History::fail(History::Failure::Discontinuity);
    m_cdHandle = isoFile;
    if (m_cdHandle->failed()) {
        m_cdHandle.reset();
        return false;
    }
    if (g_emulator->settings.get<Emulator::SettingFullCaching>() && m_cdHandle.isA<UvFile>()) {
        m_cdHandle.asA<UvFile>()->startCaching();
    }

    PCSX::g_system->printf(_("Loaded CD Image: %s"), m_isoPath.string());

    m_cddaBigEndian = false;
    m_subChanMixed = false;
    m_subChanRaw = false;
    m_pregapOffset = 0;
    m_historyPregapKnown = true;
    m_cdrIsoMultidiskCount = 1;
    m_multifile = false;

    m_useCompressed = false;
    m_cdimg_read_func = &CDRIso::cdread_normal;

    for (auto &i : m_ti) {
        i = {};
    }

    if (parsecue(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+cue]");
    } else if (parsetoc(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+toc]");
    } else if (parseccd(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+ccd]");
    } else if (parsemds(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+mds]");
    }
    // TODO Is it possible that cue/ccd+ecm? otherwise use else if below to supressn extra checks
    if (handlepbp(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[pbp]");
        m_useCompressed = true;
        m_cdimg_read_func = &CDRIso::cdread_compressed;
    } else if (handlecbin(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[cbin]");
        m_useCompressed = true;
        m_cdimg_read_func = &CDRIso::cdread_compressed;
    } else if ((handleecm(reinterpret_cast<const char *>(m_isoPath.string().c_str()), m_cdHandle, NULL))) {
        PCSX::g_system->printf("[+ecm]");
    }

    if (!m_subChanMixed && opensubfile(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+sub]");
    }
    if (opensbifile(reinterpret_cast<const char *>(m_isoPath.string().c_str()))) {
        PCSX::g_system->printf("[+sbi]");
    }

    if (!m_ecm_file_detected) {
        // guess whether it is mode1/2048
        if (m_cdHandle->size() % 2048 == 0) {
            unsigned int modeTest = m_cdHandle->readAt<uint32_t>(0);
            if (modeTest != 0xffffff00) {
                PCSX::g_system->printf("[2048]");
                m_isMode1ISO = true;
            }
        }
        m_cdHandle->rSeek(0, SEEK_SET);
    }

    if (m_numtracks == 0) {
        // We got no track information, just an iso file, so let's fill in very basic data
        m_numtracks = 1;
        m_ti[1].type = TrackType::DATA;
        m_ti[1].start = IEC60908b::MSF(0, 2, 0);
        m_ti[1].pregap = IEC60908b::MSF(0, 0, 0);
        m_ti[1].handle = m_cdHandle;
        m_ti[1].length = IEC60908b::MSF(m_ti[1].handle->size() / 2352);
    }

    if (m_ppf.load(m_isoPath)) {
        PCSX::g_system->printf("[+ppf]");
    }
    PCSX::g_system->printf(".\n");

    printTracks();

    if (m_subChanMixed && (m_cdimg_read_func == &CDRIso::cdread_normal)) {
        m_cdimg_read_func = &CDRIso::cdread_sub_mixed;
    } else if (m_isMode1ISO && (m_cdimg_read_func == &CDRIso::cdread_normal)) {
        m_cdimg_read_func = &CDRIso::cdread_2048;
    }

    // make sure we have another handle open for cdda
    if (m_numtracks > 1 && !m_ti[1].handle) {
        m_ti[1].handle.setFile(new UvFile(m_isoPath));
        if (g_emulator->settings.get<Emulator::SettingFullCaching>()) {
            m_ti[1].handle.asA<UvFile>()->startCaching();
        }
    }

    cacheStateReadOwners();
    return true;
}

void PCSX::CDRIso::close() {
    clearStateReadOwners();
    if (History::CD::active()) History::fail(History::Failure::Discontinuity);
    m_cdHandle.reset();
    m_subHandle.reset();

    if (m_compr_img) {
        free(m_compr_img->index_table);
        free(m_compr_img);
        m_compr_img = nullptr;
    }

    for (int i = 1; i <= m_numtracks; i++) {
        if (m_ti[i].handle) {
            m_ti[i].handle.reset();
            m_ti[i].cddatype = trackinfo::NONE;
        }
    }
    m_numtracks = 0;
    m_ti[1].type = TrackType::CLOSED;

    memset(m_cdbuffer, 0, sizeof(m_cdbuffer));
    m_historyBufferDefined = true;
    m_useCompressed = false;
    // ECM LUT
    free(m_ecm_savetable);
    m_ecm_savetable = nullptr;

    if (m_decoded_ecm) {
        m_decoded_ecm.reset();
        free(m_decoded_ecm_buffer);
        m_decoded_ecm_buffer = nullptr;
    }
    m_ecm_file_detected = false;
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getTD(uint8_t track) {
    if (track == 0) {
        unsigned int sect;
        sect = m_ti[m_numtracks].start.toLBA() + m_ti[m_numtracks].length.toLBA() - m_ti[m_numtracks].pregap.toLBA();
        { const auto result = IEC60908b::MSF(sect); History::CD::mediaQuery(1, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    } else if (m_numtracks > 1 && track <= m_numtracks) {
        { const auto result = IEC60908b::MSF(m_ti[track].start.toLBA()); History::CD::mediaQuery(1, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    }
    { const auto result = IEC60908b::MSF(0, 2, 0); History::CD::mediaQuery(1, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getLength(uint8_t track) {
    if (track == 0) {
        unsigned int sect;
        sect = m_ti[m_numtracks].start.toLBA() + m_ti[m_numtracks].length.toLBA();
        { const auto result = IEC60908b::MSF(sect); History::CD::mediaQuery(2, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    } else if (m_numtracks > 0 && track <= m_numtracks) {
        { const auto result = m_ti[track].length; History::CD::mediaQuery(2, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    }
    { const auto result = IEC60908b::MSF(0, 0, 0); History::CD::mediaQuery(2, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
}

PCSX::IEC60908b::MSF PCSX::CDRIso::getPregap(uint8_t track) {
    if (track <= 1) {
        { const auto result = IEC60908b::MSF(0, 0, 0); History::CD::mediaQuery(3, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    } else if (m_numtracks > 0 && track <= m_numtracks) {
        { const auto result = m_ti[track].pregap; History::CD::mediaQuery(3, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
    }
    { const auto result = IEC60908b::MSF(0, 2, 0); History::CD::mediaQuery(3, track, uint32_t(result.m) | (uint32_t(result.s)<<8) | (uint32_t(result.f)<<16)); return result; }
}

bool PCSX::CDRIso::getLocP(const PCSX::IEC60908b::MSF msf, uint8_t locP[8]) {
    // TODO: actually check subchannels from iso.
    auto length = getTD(0) + IEC60908b::MSF{0, 2, 0};
    if (msf >= length) { History::CD::mediaQuery(6, uint32_t(msf.m) | (uint32_t(msf.s)<<8) | (uint32_t(msf.f)<<16), false); return false; }
    unsigned track;
    for (track = m_numtracks; track != 1; track--) {
        if (m_ti[track].start <= msf) break;
    }

    bool inPregap = false;
    IEC60908b::MSF nextPregapStart;
    if (msf < m_ti[track].start) {
        // Before track 1 starts: its pregap, index 00, counting down.
        inPregap = true;
    } else if (track != m_numtracks) {
        if (msf >= (m_ti[track + 1].start - m_ti[track + 1].pregap)) {
            track++;
            inPregap = true;
        }
    }
    locP[0] = IEC60908b::itob(track);
    locP[1] = inPregap ? 0 : 1;
    IEC60908b::MSF relative;
    if (inPregap) {
        relative = m_ti[track].start - msf;
    } else {
        relative = msf - m_ti[track].start;
    }
    relative.toBCD(locP + 2);
    msf.toBCD(locP + 5);

    History::CD::mediaQuery(6, uint32_t(msf.m) | (uint32_t(msf.s)<<8) | (uint32_t(msf.f)<<16), true, locP, 8);
    return true;
}

unsigned PCSX::CDRIso::getTrack(const PCSX::IEC60908b::MSF msf) {
    auto length = getTD(0) + IEC60908b::MSF{0, 2, 0};
    if (msf >= length) { History::CD::mediaQuery(4, uint32_t(msf.m) | (uint32_t(msf.s)<<8) | (uint32_t(msf.f)<<16), 0); return 0; }
    unsigned track;
    for (track = m_numtracks; track != 1; track--) {
        if (m_ti[track].start <= msf) break;
    }
    IEC60908b::MSF nextPregapStart;
    if (track != m_numtracks) {
        if (msf >= (m_ti[track + 1].start - m_ti[track + 1].pregap)) {
            track++;
        }
    }
    History::CD::mediaQuery(4, uint32_t(msf.m) | (uint32_t(msf.s)<<8) | (uint32_t(msf.f)<<16), track);
    return track;
}

// Decode 'raw' subchannel data from being packed bitwise.
// Essentially is a bitwise matrix transposition.
void PCSX::CDRIso::decodeRawSubData() {
    unsigned char subQData[12];
    memset(subQData, 0, sizeof(subQData));

    for (int i = 0; i < 8 * 12; i++) {
        if (m_subbuffer.raw[i] & (1 << 6)) {  // only subchannel Q is needed
            subQData[i >> 3] |= (1 << (7 - (i & 7)));
        }
    }

    memcpy(&m_subbuffer.Q, subQData, 12);
}

// read track
bool PCSX::CDRIso::readTrack(const IEC60908b::MSF time) {
    int sector = time.toLBA() - 150;
    long ret;

    const bool present = bool(m_cdHandle);
    const bool unavailable = present && m_cdHandle->failed();
    History::CD::predicate(History::CD::Predicate::Availability,
        {uint32_t(present), uint32_t(present), uint32_t(unavailable)});
    if (!present || unavailable) {
        PCSX::History::CD::mediaOutcome(5, 0);
        return false;
    }
    if (m_pregapOffset) {
        m_subChanMissing = false;
        if (sector >= m_pregapOffset) {
            sector -= 2 * 75;
            if (sector < m_pregapOffset) m_subChanMissing = true;
        }
    }
    if (History::CD::active()) History::CD::predicate(History::CD::Predicate::SubchannelState,
        {1, uint32_t(bool(m_subHandle)), uint32_t(m_subChanMixed), uint32_t(m_subChanMissing)});

    ret = (*this.*m_cdimg_read_func)(m_cdHandle, 0, m_cdbuffer, sector);
    if (ret < 0) {
        if (History::CD::active()) History::CD::mediaOutcome(6, ret, 0, 0, uint32_t(sector),
            12 | (m_useCompressed ? 1 : 0) | (m_isMode1ISO ? 16 : 0) | (m_subChanMixed ? 32 : 0));
        return false;
    }

    if (m_subHandle) {
        m_subHandle->rSeek(sector * IEC60908b::SUB_FRAMESIZE, SEEK_SET);
        m_subHandle->read(m_subbuffer.raw, IEC60908b::SUB_FRAMESIZE);

        if (m_subChanRaw) decodeRawSubData();
    }

    m_ppf.maybePatchSector(m_cdbuffer, time);

    if (History::CD::active()) History::CD::mediaOutcome(7, ret, 0, 0, uint32_t(sector),
        12 | (m_useCompressed ? 1 : 0) | (m_isMode1ISO ? 16 : 0) | (m_subChanMixed ? 32 : 0));
    return true;
}

unsigned PCSX::CDRIso::readSectors(uint32_t lba, void *buffer_, unsigned count) {
    if (History::CD::active() && !History::CD::isoScan()) History::fail(History::Failure::Association);
    unsigned actual = 0;
    uint8_t *buffer = reinterpret_cast<uint8_t *>(buffer_);

    if (m_cdHandle->failed()) {
        return 0;
    }

    for (unsigned i = 0; i < count; i++) {
        auto ptr = buffer + actual * IEC60908b::FRAMESIZE_RAW;
        History::CD::Media historyMedium(3, 0, lba, History::CD::backing());
        if (lba < m_ti[1].length.toLBA()) {
            IEC60908b::MSF time(lba + 150);
            long ret = (*this.*m_cdimg_read_func)(m_cdHandle, 0, ptr, lba++);
            m_ppf.maybePatchSector(ptr, time);
            History::CD::mediaOutcome(ret < 0 ? 6 : 7, ret, 0, 0, lba-1,
                12 | (m_isMode1ISO ? 16 : 0) | (m_subChanMixed ? 32 : 0));
            historyMedium.result(ret >= 0, 0);
            if (ret < 0) return actual;
            if (History::CD::active() && ret != (m_isMode1ISO ? 2048 : IEC60908b::FRAMESIZE_RAW)) History::fail(History::Failure::Bounds);
            else History::CD::dataMutation(8, 0, IEC60908b::FRAMESIZE_RAW, ptr, historyMedium.id());
        } else {
            const bool read = readCDDA(IEC60908b::MSF(lba++), ptr);
            historyMedium.result(read, 0);
            History::CD::dataMutation(8, 0, IEC60908b::FRAMESIZE_RAW, ptr, historyMedium.id());
            if (!read) return actual;
        }
        actual++;
    }

    return actual;
}

// gets subchannel data
const PCSX::IEC60908b::Sub *PCSX::CDRIso::getBufferSub() {
    if (History::CD::active()) History::CD::predicate(History::CD::Predicate::SubchannelState,
        {2, uint32_t(bool(m_subHandle)), uint32_t(m_subChanMixed), uint32_t(m_subChanMissing)});
    if ((m_subHandle || m_subChanMixed) && !m_subChanMissing) {
        return &m_subbuffer;
    }

    return nullptr;
}

// read CDDA sector into buffer
bool PCSX::CDRIso::readCDDA(IEC60908b::MSF msf, unsigned char *buffer) {
    unsigned int file, track, track_start = 0;
    int ret;

    uint32_t lba = msf.toLBA();

    // find current track index
    for (track = m_numtracks;; track--) {
        track_start = m_ti[track].start.toLBA();
        if (track_start <= lba) break;
        if (track == 1) break;
    }

    // data tracks play silent
    if (m_ti[track].type != TrackType::CDDA) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        PCSX::History::CD::mediaOutcome(1, 0, track);
        return true;
    }

    // if above the track's end (likely next track's pregap), play silence
    if (lba >= (track_start + m_ti[track].length.toLBA())) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        PCSX::History::CD::mediaOutcome(2, 0, track);
        return true;
    }

    file = 1;
    if (m_multifile) {
        // find the file that contains this track
        for (file = track; file > 1; file--) {
            if (m_ti[file].handle) break;
        }
    }

    ret = (*this.*m_cdimg_read_func)(m_ti[file].handle, m_ti[track].start_offset, buffer, lba - track_start);
    if (ret != IEC60908b::FRAMESIZE_RAW) {
        memset(buffer, 0, IEC60908b::FRAMESIZE_RAW);
        if (History::CD::active()) History::CD::mediaOutcome(3, ret, track, file, lba-track_start,
            8 | (m_useCompressed ? 5 : 0) | (m_isMode1ISO ? 16 : 0) | (m_subChanMixed ? 32 : 0),
            m_ti[track].start_offset);
        return false;
    }

    if (m_cddaBigEndian) {
        unsigned char tmp;

        for (int i = 0; i < IEC60908b::FRAMESIZE_RAW / 2; i++) {
            tmp = buffer[i * 2];
            buffer[i * 2] = buffer[i * 2 + 1];
            buffer[i * 2 + 1] = tmp;
        }
    }

    if (History::CD::active()) History::CD::mediaOutcome(4, ret, track, file, lba-track_start,
        8 | (m_useCompressed ? 5 : 0) | (m_cddaBigEndian ? 2 : 0) |
        (m_isMode1ISO ? 16 : 0) | (m_subChanMixed ? 32 : 0), m_ti[track].start_offset);
    return true;
}

bool PCSX::CDRIso::failed() { return !m_cdHandle && !m_ecm_savetable && !m_decoded_ecm; }

uint32_t PCSX::CDRIso::historyFile(File* file, uint32_t depth) {
    if (!file) return 0;
    if (depth > 4) throw std::runtime_error("CD file graph depth");
    for (uint32_t n = 0; n < m_historyFileCount; ++n) {
        if (m_historyFiles[n] != file) continue;
        if (depth + m_historyDepths[n] > 4) throw std::runtime_error("CD cached file graph depth");
        return n+1;
    }
    if (m_historyFileCount == HistoryFiles) throw std::runtime_error("CD file graph bound");
    size_t provedBytes = 0;
    if (file->stateReadExtent(provedBytes) != File::RawSnapshotStatus::Ok || !file->stateReadAvailable())
        throw std::runtime_error("unsupported CD backing capacity or completion");
    uint32_t parent = 0, kind = 1;
    uint64_t start = 0, size = provedBytes;
    std::u8string path;
    if (auto* slice = dynamic_cast<SubFile*>(file)) {
        kind = 2;
        parent = historyFile(slice->m_file ? slice->m_file.operator->() : nullptr, depth+1);
        if (!parent) throw std::runtime_error("CD slice without backing file");
        start = slice->m_start;
        if (size != slice->m_size) throw std::runtime_error("CD view extent differs from proved capacity");
    } else if (dynamic_cast<UvFile*>(file)) {
        path = file->filename().generic_u8string();
        if (path.empty() || path.size() > 4096 || path.find(u8'\0') != path.npos)
            throw std::runtime_error("CD backing path bound");
    } else {
        throw std::runtime_error("unsupported CD backing file");
    }
    if (m_historyFileCount == HistoryFiles) throw std::runtime_error("CD file graph bound");
    const uint32_t ordinal = ++m_historyFileCount;
    m_historyFiles[ordinal-1] = file;
    m_historyDepths[ordinal-1] = parent ? m_historyDepths[parent-1]+1 : 0;
    auto word = [&](uint32_t value) {
        for (unsigned b = 0; b < 4; ++b) m_historyImage.push_back(value >> (b*8));
    };
    word(ordinal); word(kind); word(parent); word(path.size());
    word(start); word(start >> 32); word(size); word(size >> 32);
    m_historyImage.insert(m_historyImage.end(), path.begin(), path.end());
    return ordinal;
}

bool PCSX::CDRIso::historyGeometry(std::array<uint32_t, HistoryWords>& words) {
    if (m_numtracks < 0 || m_numtracks >= int(MAXTRACKS) || m_useCompressed || m_compr_img ||
        m_ecm_file_detected || m_ecm_savetable || m_decoded_ecm || m_subHandle ||
        m_subChanMixed || m_subChanRaw || !m_ppf.empty() || (m_historySbiKnown && sbicount)) return false;
    uint32_t backend = 0;
    if (m_cdimg_read_func == &CDRIso::cdread_normal) backend = 1;
    else if (m_cdimg_read_func == &CDRIso::cdread_2048) backend = 2;
    else if (m_cdimg_read_func) return false;
    if (m_cdHandle && (!backend || !m_historyPregapKnown)) return false;
    auto ordinal = [&](IO<File>& handle) {
        if (!handle) return uint32_t(0);
        auto* file = handle.operator->();
        for (uint32_t n = 0; n < m_historyFileCount; ++n)
            if (m_historyFiles[n] == file) return n+1;
        return UINT32_MAX;
    };
    const auto main = ordinal(m_cdHandle);
    if (main == UINT32_MAX) return false;
    words = {};
    words[0] = m_numtracks; words[1] = MAXTRACKS; words[2] = m_historyPregapKnown;
    words[3] = m_historyPregapKnown ? m_pregapOffset : 0;
    words[4] = m_multifile; words[5] = m_isMode1ISO; words[6] = m_cddaBigEndian;
    words[7] = backend; words[8] = main; words[13] = m_historySbiKnown;
    words[HistoryPpfEmpty] = 1;
    const auto msf = [](IEC60908b::MSF value) {
        return uint32_t(value.m) | (uint32_t(value.s) << 8) | (uint32_t(value.f) << 16);
    };
    for (unsigned n = 0; n < MAXTRACKS; ++n) {
        auto& track = m_ti[n];
        const auto file = ordinal(track.handle);
        if (file == UINT32_MAX) return false;
        const unsigned first = 16 + n*7;
        words[first] = uint32_t(track.type); words[first+1] = msf(track.start);
        words[first+2] = msf(track.pregap); words[first+3] = msf(track.length);
        words[first+4] = track.start_offset; words[first+5] = track.cddatype; words[first+6] = file;
    }
    return true;
}

bool PCSX::CDRIso::historyPrepare() {
    if (History::active() || !History::CD::idle()) return false;
    m_historyFileCount = 0; m_historyFiles.fill(nullptr);
    m_historyImage.clear();
    try {
        m_historyImage.resize(HistoryWords * 4);
        historyFile(m_cdHandle ? m_cdHandle.operator->() : nullptr, 0);
        for (auto& track : m_ti) historyFile(track.handle ? track.handle.operator->() : nullptr, 0);
        if (!historyGeometry(m_historyGeometry)) { m_historyImage.clear(); return false; }
        for (unsigned n = 0; n < HistoryWords; ++n)
            for (unsigned b = 0; b < 4; ++b) m_historyImage[n*4+b] = m_historyGeometry[n] >> (b*8);
    } catch (...) {
        m_historyImage.clear(); return false;
    }
    return true;
}

bool PCSX::CDRIso::historyValidate() {
    std::array<uint32_t, HistoryWords> geometry{};
    return !m_historyImage.empty() && historyGeometry(geometry) && geometry == m_historyGeometry;
}

void PCSX::CDRIso::history(uint32_t phase) {
    if (!History::CD::active()) return;
    if (phase > 1 || !historyValidate()) { History::fail(History::Failure::Discontinuity); return; }
    History::emit(History::Kind::CdImage, phase, {3, uint32_t(HistoryWords), m_historyFileCount, MAXTRACKS},
        0, m_historyImage.data(), m_historyImage.size(), 1);
}

void PCSX::CDRIso::clearStateReadOwners() noexcept {
    for (auto& owner : m_stateReadOwners) {
        owner.file.reset();
        owner.bytes = 0;
        owner.available = false;
    }
    m_stateReadOwnerCount = 0;
}

void PCSX::CDRIso::cacheStateReadOwners() noexcept {
    clearStateReadOwners();
    const auto add = [this](IO<File> file) {
        if (!file || m_stateReadOwnerCount == m_stateReadOwners.size()) return;
        for (size_t i = 0; i < m_stateReadOwnerCount; ++i) {
            if (m_stateReadOwners[i].file.operator->() == file.operator->()) return;
        }
        auto& owner = m_stateReadOwners[m_stateReadOwnerCount++];
        owner.file = file;
        owner.available = file->stateReadExtent(owner.bytes) == File::RawSnapshotStatus::Ok;
    };
    add(m_cdHandle);
    add(m_subHandle);
    if (m_numtracks < MAXTRACKS) {
        for (unsigned i = 1; i <= m_numtracks; ++i) add(m_ti[i].handle);
    }
}

bool PCSX::CDRIso::acceptsStateFile(IO<File> file, uint64_t start, uint64_t length) const noexcept {
    if (!file || start > uint64_t(std::numeric_limits<ssize_t>::max()) ||
        length > uint64_t(std::numeric_limits<ssize_t>::max()) - start) return false;
    for (size_t i = 0; i < m_stateReadOwnerCount; ++i) {
        auto owner = m_stateReadOwners[i].file;
        if (owner && owner.operator->() == file.operator->()) {
            const auto& proof = m_stateReadOwners[i];
            return proof.available && file->stateReadAvailable() && start <= proof.bytes &&
                   length <= proof.bytes - start;
        }
    }
    return false;
}

bool PCSX::CDRIso::acceptsStateSector(IO<File> file, uint32_t base, int64_t sector) const noexcept {
    if (sector < 0 || sector > std::numeric_limits<int>::max()) return false;
    uint64_t stride = 0;
    if (m_cdimg_read_func == &CDRIso::cdread_normal) stride = IEC60908b::FRAMESIZE_RAW;
    else if (m_cdimg_read_func == &CDRIso::cdread_sub_mixed)
        stride = IEC60908b::FRAMESIZE_RAW + IEC60908b::SUB_FRAMESIZE;
    else if (m_cdimg_read_func == &CDRIso::cdread_2048) {
        if (sector > std::numeric_limits<int>::max() / 2048 ||
            sector > std::numeric_limits<int>::max() - 150) return false;
        stride = 2048;
    } else return false;
    const uint64_t start = uint64_t(base) + uint64_t(sector) * stride;
    return acceptsStateFile(file, start, stride);
}

bool PCSX::CDRIso::statePosition(IEC60908b::MSF at, unsigned& track) const noexcept {
    if (m_numtracks >= MAXTRACKS) return false;
    const auto& last = m_ti[m_numtracks];
    const auto end = IEC60908b::MSF(last.start.toLBA() + last.length.toLBA() - last.pregap.toLBA()) +
                     IEC60908b::MSF{0, 2, 0};
    if (at >= end) {
        track = 0;
        return true;
    }
    if (m_numtracks == 0) return false;
    track = m_numtracks;
    while (track != 1 && m_ti[track].start > at) --track;
    if (track != m_numtracks && at >= m_ti[track + 1].start - m_ti[track + 1].pregap) ++track;
    return true;
}

bool PCSX::CDRIso::acceptsStateAccess(StateAccess kind, IEC60908b::MSF at, uint8_t requestedTrack) const noexcept {
    if (m_numtracks >= MAXTRACKS) return false;
    if (kind == StateAccess::TrackCount || kind == StateAccess::TrackAddress) return true;
    if (kind == StateAccess::AudioSector && at >= getTD(0)) return true;
    unsigned track = 0;
    if (!statePosition(at, track)) return false;
    if (kind == StateAccess::Position || track == 0) return true;
    if (kind == StateAccess::DataSector) {
        if (m_ti[track].type == TrackType::CDDA) return true;
        int64_t sector = int64_t(at.toLBA()) - 150;
        if (sector < 0) return false;
        if (m_pregapOffset && uint64_t(sector) >= m_pregapOffset) sector -= 150;
        if (!acceptsStateSector(m_cdHandle, 0, sector) || !m_ppf.acceptsStateRead()) return false;
        return !m_subHandle || acceptsStateFile(m_subHandle, uint64_t(sector) * IEC60908b::SUB_FRAMESIZE,
                                               IEC60908b::SUB_FRAMESIZE);
    }
    if (kind != StateAccess::AudioSector) return false;
    const uint32_t lba = at.toLBA();
    track = m_numtracks;
    while (track > 1 && m_ti[track].start.toLBA() > lba) --track;
    const uint32_t start = m_ti[track].start.toLBA();
    if (m_ti[track].type != TrackType::CDDA || lba >= start + m_ti[track].length.toLBA()) return true;
    if (lba < start) return false;
    unsigned file = 1;
    if (m_multifile) {
        file = track;
        while (file > 1 && !m_ti[file].handle) --file;
    }
    return acceptsStateSector(m_ti[file].handle, m_ti[track].start_offset, int64_t(lba) - start);
}

uint8_t PCSX::CDRIso::getTN() {
    const uint8_t result = std::max(m_numtracks, 1U);
    History::CD::mediaQuery(0, 0, result); return result;
}
PCSX::CDRIso::TrackType PCSX::CDRIso::getTrackType(unsigned track) {
    const auto result = m_ti[track].type;
    History::CD::mediaQuery(5, track, uint32_t(result)); return result;
}
