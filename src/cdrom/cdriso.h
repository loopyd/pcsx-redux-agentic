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

#pragma once

#include <stdio.h>
#include <zlib.h>

#include <filesystem>
#include <array>
#include <array>
#include <vector>

#include "cdrom/ppf.h"
#include "core/psxemulator.h"
#include "support/uvfile.h"
#include "supportpsx/iec-60908b.h"

namespace PCSX {

class CDRIso {
  public:
    CDRIso(const std::filesystem::path& path) : CDRIso() {
        m_isoPath = path;
        open(new UvFile(m_isoPath));
    }
    CDRIso(IO<File> isoFile) : CDRIso() {
        m_isoPath = isoFile->filename();
        open(isoFile);
    }
    ~CDRIso() {
        close();
        inflateEnd(&m_zstr);
    }
    enum class TrackType { CLOSED = 0, DATA = 1, CDDA = 2 };
    TrackType getTrackType(unsigned track);
    const std::filesystem::path& getIsoPath() { return m_isoPath; }
    uint8_t getTN();
    IEC60908b::MSF getTD(uint8_t track);
    IEC60908b::MSF getLength(uint8_t track);
    IEC60908b::MSF getPregap(uint8_t track);
    bool getLocP(const IEC60908b::MSF msf, uint8_t locP[8]);
    unsigned getTrack(const IEC60908b::MSF msf);

    bool failed();
    enum class StateAccess { TrackCount, TrackAddress, Position, DataSector, AudioSector };
    bool acceptsStateAccess(StateAccess kind, IEC60908b::MSF at, uint8_t track = 0) const noexcept;

    /* Probably have to change those */
    bool readTrack(const IEC60908b::MSF time);
    unsigned readSectors(uint32_t lba, void* buffer, unsigned count);
    uint8_t* getBuffer();
    bool historyBufferDefined() const { return m_historyBufferDefined; }
    const IEC60908b::Sub* getBufferSub();
    bool readCDDA(const IEC60908b::MSF msf, unsigned char* buffer);
    PPF* getPPF() { return &m_ppf; }

    unsigned m_cdrIsoMultidiskCount;
    unsigned m_cdrIsoMultidiskSelect;

    bool CheckSBI(const uint8_t* time);
    bool historyPrepare();
    bool historyValidate();
    uint32_t historyBytes() const { return m_historyImage.size(); }
    void history(uint32_t phase);

  private:
    CDRIso();
    bool open(IO<File> isoFile);
    void close();

    struct StateReadOwner {
        IO<File> file;
        size_t bytes = 0;
        bool available = false;
    };
    std::array<StateReadOwner, 102> m_stateReadOwners;
    size_t m_stateReadOwnerCount = 0;
    void clearStateReadOwners() noexcept;
    void cacheStateReadOwners() noexcept;
    bool acceptsStateFile(IO<File> file, uint64_t start, uint64_t length) const noexcept;
    bool acceptsStateSector(IO<File> file, uint32_t base, int64_t sector) const noexcept;
    bool statePosition(IEC60908b::MSF at, unsigned& track) const noexcept;

    std::filesystem::path m_isoPath;
    typedef ssize_t (CDRIso::*read_func_t)(IO<File> f, unsigned int base, void* dest, int sector);

    bool m_useCompressed = false;
    z_stream m_zstr;

    IO<File> m_cdHandle;
    IO<File> m_subHandle;

    bool m_subChanMixed = false;
    bool m_subChanRaw = false;
    bool m_subChanMissing = false;

    bool m_multifile = false;
    bool m_isMode1ISO = false;  // TODO: use sector size/mode info from CUE also?

    uint8_t m_cdbuffer[2352];
    bool m_historyBufferDefined = false;
    IEC60908b::Sub m_subbuffer;

    bool m_cddaBigEndian = false;
    /* Frame offset into CD image where pregap data would be found if it was there.
     * If a game seeks there we must *not* return subchannel data since it's
     * not in the CD image, so that cdrom code can fake subchannel data instead.
     * XXX: there could be multiple pregaps but PSX dumps only have one? */
    unsigned int m_pregapOffset;
    bool m_historyPregapKnown = false;

    // compressed image stuff
    struct compr_img_t {
        unsigned char buff_raw[16][2352];
        unsigned char buff_compressed[2352 * 16 + 100];
        unsigned int* index_table;
        unsigned int index_len;
        unsigned int block_shift;
        unsigned int current_block;
        unsigned int sector_in_blk;
    }* m_compr_img = nullptr;

    read_func_t m_cdimg_read_func = nullptr;

    uint32_t m_len_decoded_ecm_buffer = 0;  // same as decoded ECM file length or 2x size
    uint32_t m_len_ecm_savetable = 0;       // same as sector count of decoded ECM file or 2x count

    uint32_t m_decoded_ecm_sectors = 0;  // disabled

    bool m_ecm_file_detected = false;
    uint32_t m_prevsector;

    IO<File> m_decoded_ecm = nullptr;
    void* m_decoded_ecm_buffer = nullptr;

    // Function that is used to read CD normally
    read_func_t m_cdimg_read_func_o = nullptr;

    struct ECMFILELUT {
        int32_t sector;
        int32_t filepos;
    };

    ECMFILELUT* m_ecm_savetable = nullptr;

    static inline const size_t ECM_SECTOR_SIZE[4] = {1, 2352, 2336, 2336};

    struct trackinfo {
        TrackType type = TrackType::CLOSED;
        IEC60908b::MSF pregap;
        IEC60908b::MSF start;
        IEC60908b::MSF length;
        IO<File> handle = nullptr;                                       // for multi-track images CDDA
        enum CDDAType { NONE = 0, BIN = 1, CCDDA = 2 } cddatype = NONE;  // BIN, WAV, MP3, APE
        uint32_t start_offset = 0;                                       // byte offset from start of above file
    };

    static constexpr unsigned MAXTRACKS = 100; /* How many tracks can a CD hold? */

    unsigned m_numtracks = 0;
    struct trackinfo m_ti[MAXTRACKS];

    // redump.org SBI files
    uint8_t sbitime[256][3], sbicount;
    bool m_historySbiKnown = false;
    PPF m_ppf;

    enum HistoryHeaderWord : size_t { HistoryPpfEmpty = 14 };
    static constexpr size_t HistoryWords = 16 + MAXTRACKS * 7;
    static constexpr size_t HistoryFiles = 202;
    std::array<uint32_t, HistoryWords> m_historyGeometry{};
    std::array<File*, HistoryFiles> m_historyFiles{};
    std::array<uint32_t, HistoryFiles> m_historyDepths{};
    uint32_t m_historyFileCount = 0;
    std::vector<uint8_t> m_historyImage;
    bool historyGeometry(std::array<uint32_t, HistoryWords>& words);
    uint32_t historyFile(File* file, uint32_t depth);

    void decodeRawSubData();
    bool parsetoc(const char* isofile);
    bool parsecue(const char* isofile);
    bool parseccd(const char* isofile);
    bool parsemds(const char* isofile);
    bool handlepbp(const char* isofile);
    bool handlecbin(const char* isofile);
    bool handleecm(const char* isoname, IO<File> cdh, int32_t* accurate_length);
    bool opensubfile(const char* isoname);
    bool opensbifile(const char* isoname);

    bool LoadSBI(const char* filename);

    ssize_t cdread_normal(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_sub_mixed(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_compressed(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t cdread_2048(IO<File> f, unsigned int base, void* dest, int sector);
    ssize_t ecmDecode(IO<File> f, unsigned int base, void* dest, int sector);

    void printTracks();
    void UnloadSBI();
};

}  // namespace PCSX
