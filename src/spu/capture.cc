// SPDX-License-Identifier: GPL-2.0-or-later
#include "spu/sdlaudio.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "core/psxemulator.h"
#include "core/psxmem.h"
#include "core/r3000a.h"
#include "core/system.h"
#include "lua/luawrapper.h"

namespace {
class StreamGuard {
    SDL_AudioStream* m_stream;
  public:
    explicit StreamGuard(SDL_AudioStream* stream) : m_stream(stream), valid(!stream || SDL_LockAudioStream(stream)) {}
    ~StreamGuard() { if (valid && m_stream) SDL_UnlockAudioStream(m_stream); }
    bool valid;
};
bool paused(const PCSX::SPU::Capture& capture) {
    return capture.ownerThread() && !PCSX::g_system->running() && !PCSX::g_system->quitting();
}
uint32_t integer(PCSX::Lua L, int index, uint32_t maximum) {
    const auto value = L.checknumber(index);
    if (!std::isfinite(value) || value < 0 || value > maximum || std::floor(value) != value) {
        L.error("audio argument must be an integer within its documented bound");
        return 0;
    }
    return uint32_t(value);
}
void number(PCSX::Lua L, const char* key, int64_t value) { L.push(key); L.push(lua_Number(value)); L.settable(); }
void wide(PCSX::Lua L, const char* key, uint64_t value) { L.push(key); L.push(std::to_string(value)); L.settable(); }
void string(PCSX::Lua L, const char* key, std::string_view value) { L.push(key); L.push(value); L.settable(); }

bool captureName(std::string_view name) {
    const auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (name.empty() || name.size() > 255 || !alnum(name.front())) return false;
    for (char c : name) if (!alnum(c) && c != '_' && c != '.' && c != '-') return false;
    return true;
}

std::array<uint8_t, 58> waveHeader(uint32_t frames) {
    std::array<uint8_t, 58> bytes{};
    const auto integer = [&](uint32_t offset, uint32_t value, uint32_t width) {
        for (uint32_t n = 0; n < width; ++n) bytes[offset + n] = uint8_t(value >> (n * 8));
    };
    std::memcpy(bytes.data(), "RIFF", 4); integer(4, 50 + frames * 8, 4);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); integer(16, 18, 4);
    integer(20, 3, 2); integer(22, 2, 2); integer(24, 44100, 4); integer(28, 352800, 4);
    integer(32, 8, 2); integer(34, 32, 2);
    std::memcpy(bytes.data() + 38, "fact", 4); integer(42, 4, 4); integer(46, frames, 4);
    std::memcpy(bytes.data() + 50, "data", 4); integer(54, frames * 8, 4);
    return bytes;
}

const char* exportPCM(PCSX::SPU::Capture& capture, SDL_AudioStream* stream,
                      const char* rawPath, const char* wavPath, uint32_t frames) {
    auto* raw = std::fopen(rawPath, "wb");
    if (!raw) return "audio PCM export could not open raw capture";
    auto* wav = std::fopen(wavPath, "wb");
    const char* error = wav ? nullptr : "audio PCM export could not open WAV capture";
    if (!error) {
        const auto header = waveHeader(frames);
        if (std::fwrite(header.data(), 1, header.size(), wav) != header.size()) error = "audio PCM export WAV header write failed";
    }
    std::array<float, PCSX::SPU::Capture::MAX_CHUNK * 2> samples;
    std::array<uint8_t, PCSX::SPU::Capture::MAX_CHUNK * 8> bytes;
    for (uint32_t offset = 0; offset < frames && !error;) {
        const uint32_t count = std::min(PCSX::SPU::Capture::MAX_CHUNK, frames - offset);
        bool okay = false;
        {
            StreamGuard lock(stream);
            if (lock.valid) okay = capture.samples(offset, count, samples.data());
        }
        if (!okay) { error = "audio PCM export requires frozen capture and stream exclusion"; break; }
        for (uint32_t n = 0; n < count * 2; ++n) {
            uint32_t bits;
            std::memcpy(&bits, &samples[n], sizeof(bits));
            for (uint32_t b = 0; b < 4; ++b) bytes[n * 4 + b] = uint8_t(bits >> (b * 8));
        }
        const size_t length = size_t(count) * 8;
        if (std::fwrite(bytes.data(), 1, length, raw) != length) error = "audio PCM export raw write failed";
        else if (std::fwrite(bytes.data(), 1, length, wav) != length) error = "audio PCM export WAV write failed";
        offset += count;
    }
    if (std::fclose(raw) != 0 && !error) error = "audio PCM export raw close failed";
    if (wav && std::fclose(wav) != 0 && !error) error = "audio PCM export WAV close failed";
    return error;
}
}

std::array<int64_t, 14> PCSX::SPU::SDLAudio::captureSettings() const {
    const auto voices = m_influences ? m_influences() : std::array<uint32_t, 2>{};
    return {m_settings.get<Volume>().value, m_settings.get<Interpolation>().value, m_settings.get<KeyOnDelay>().value,
        m_settings.get<Mute>().value, m_settings.get<Mono>().value, m_settings.get<Streaming>().value,
        m_settings.get<NullSync>().value, Environment::SETTINGS_VERSION, 0, m_settings.get<Speed>().value,
        g_emulator->settings.get<Emulator::SettingSpuIrq>().value, voices[0], voices[1],
        g_emulator->settings.get<Emulator::SettingXa>().value};
}

bool PCSX::SPU::SDLAudio::captureEnvironment(Environment& value) {
    if (!m_capture.ownerThread()) { m_capture.fail(Capture::Thread); return false; }
    const auto& backend = m_settings.get<Backend>().value;
    const auto& device = m_settings.get<Device>().value;
    if (backend.size() > 1024 || device.size() > 1024) return false;
    const auto settings = captureSettings();
    if (settings[Environment::Volume] < 0 || settings[Environment::Volume] > 3 ||
        settings[Environment::Interpolation] < 0 || settings[Environment::Interpolation] > 3) return false;
    value = {settings, backend, device, m_audioInitialized, m_stream != nullptr};
    return true;
}

bool PCSX::SPU::SDLAudio::checkEnvironment(const Environment& value) {
    if (!m_capture.ownerThread()) { m_capture.fail(Capture::Thread); return false; }
    return value.settings == captureSettings() && value.backend == m_settings.get<Backend>().value &&
        value.device == m_settings.get<Device>().value && value.initialized == m_audioInitialized &&
        value.stream == (m_stream != nullptr);
}

bool PCSX::SPU::SDLAudio::captureSnapshot(Capture::Snapshot& out) {
    if (!paused(m_capture)) return false;
    checkCaptureSettings();
    StreamGuard lock(m_stream);
    return lock.valid && m_capture.snapshot(out);
}

void PCSX::SPU::SDLAudio::checkCaptureSettings() {
    if (!m_capture.ownerThread()) { m_capture.fail(Capture::Thread); return; }
    if (!m_capture.active()) return;
    if (g_emulator->m_cpu->isDynarec() || g_emulator->m_mem->msanInitialized()) {
        m_capture.fail(Capture::Discontinuity);
        return;
    }
    if (captureSettings() != m_captureSettings || m_settings.get<Backend>().value != m_captureBackend ||
        m_settings.get<Device>().value != m_captureDevice) m_capture.fail(Capture::Discontinuity);
}

void PCSX::SPU::SDLAudio::setCaptureLua(Lua L, std::function<std::array<uint32_t, 2>()> influences) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    static_assert(VoiceStream::BUFFER_SIZE == Capture::MAX_CHUNK);
    m_influences = std::move(influences);
    L.getfieldtable("PCSX", LUA_GLOBALSINDEX);
    L.getfieldtable("Audio");
    L.declareFunc("begin", [this](Lua L) -> int {
        if (L.gettop() != 3) return L.error("Audio.begin requires frame, block and event limits");
        const auto frames = integer(L, 1, Capture::MAX_FRAMES);
        const auto blocks = integer(L, 2, Capture::MAX_RECORDS);
        const auto events = integer(L, 3, Capture::MAX_RECORDS);
        if (!frames || !blocks || !events || !paused(m_capture) || !m_stream || !m_audioInitialized ||
            g_emulator->m_cpu->isDynarec() || g_emulator->m_mem->msanInitialized()) return L.error("audio begin requires paused interpreter, initialized stream and positive limits without MSAN");
        const auto settings = captureSettings();
        if (settings[Environment::Volume] < 0 || settings[Environment::Volume] > 3 ||
            settings[Environment::Interpolation] < 0 || settings[Environment::Interpolation] > 3) return L.error("unsupported mixer settings");
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) {
                Capture::Status before;
                if (m_capture.status(before) && before.state == 0) {
                    m_captureSettings = settings;
                    m_captureBackend = m_settings.get<Backend>().value;
                    m_captureDevice = m_settings.get<Device>().value;
                    const auto driver = SDL_GetCurrentAudioDriver();
                    m_captureDriver = driver ? driver : "unavailable";
                    m_captureInitial = {uint32_t(m_voicesStream.buffered()), uint32_t(m_audioStream.buffered())};
                    m_captureFinal = {};
                    okay = m_capture.begin(frames, blocks, events, g_emulator->m_cpu->m_regs.cycle);
                }
            }
        }
        if (!okay) return L.error("audio begin rejected: recorder not empty, stream lock or allocation failed");
        return 0;
    }, -1);
    L.declareFunc("stop", [this](Lua L) -> int {
        if (L.gettop() || !paused(m_capture)) return L.error("audio stop requires paused owner and no arguments");
        checkCaptureSettings();
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid && m_capture.active()) {
                m_captureFinal = {uint32_t(m_voicesStream.buffered()), uint32_t(m_audioStream.buffered())};
                okay = m_capture.stop(g_emulator->m_cpu->m_regs.cycle);
            }
        }
        if (!okay) return L.error("audio stop requires an active recorder and stream exclusion");
        return 0;
    }, -1);
    L.declareFunc("clear", [this](Lua L) -> int {
        if (L.gettop() || !paused(m_capture)) return L.error("audio clear requires paused owner and no arguments");
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.clear();
        }
        if (!okay) return L.error("audio clear requires an inactive recorder and stream exclusion");
        return 0;
    }, -1);
    L.declareFunc("status", [this](Lua L) -> int {
        if (L.gettop() || !m_capture.ownerThread()) return L.error("audio status requires owner and no arguments");
        checkCaptureSettings();
        Capture::Status result;
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.status(result);
        }
        if (!okay) return L.error("audio status requires stream exclusion");
        L.newtable();
        number(L, "version", result.version); number(L, "state", result.state);
        wide(L, "epoch", m_capture.identity()[1]);
        number(L, "rate", 44100); number(L, "channels", 2);
        string(L, "format", "f32le"); string(L, "stage", "post-mixer-mute-mono/pre-SDL-conversion");
        number(L, "frames", result.frames); number(L, "blocks", result.blocks); number(L, "events", result.events);
        number(L, "frame_limit", result.frameLimit); number(L, "block_limit", result.blockLimit);
        number(L, "event_limit", result.eventLimit); number(L, "failures", result.failures);
        number(L, "first_failure", result.firstFailure); number(L, "dropped_events", result.droppedEvents);
        wide(L, "begin_cycle", result.beginCycle); wide(L, "end_cycle", result.endCycle);
        wide(L, "dropped_frames", result.droppedFrames); wide(L, "streaming_dropped", result.streamingDropped);
        if (result.state) {
            number(L, "initial_voice_queue", m_captureInitial[0]); number(L, "initial_disc_queue", m_captureInitial[1]);
            if (result.state == 2) {
                number(L, "final_voice_queue", m_captureFinal[0]); number(L, "final_disc_queue", m_captureFinal[1]);
            }
            L.push("settings"); L.newtable();
            for (unsigned n = 0; n < m_captureSettings.size(); ++n) number(L, Environment::settingNames[n], m_captureSettings[n]);
            string(L, "configured_backend", m_captureBackend); string(L, "configured_device", m_captureDevice);
            string(L, "active_driver", m_captureDriver);
            L.settable();
        }
        return 1;
    }, -1);
    L.declareFunc("blocks", [this](Lua L) -> int {
        if (L.gettop() != 2 || !paused(m_capture)) return L.error("audio blocks require paused owner, first index and count");
        const auto first = integer(L, 1, Capture::MAX_RECORDS);
        const auto count = integer(L, 2, Capture::MAX_BATCH);
        auto* results = static_cast<Capture::Block*>(L.newuser(size_t(count) * sizeof(Capture::Block)));
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.blocks(first, count, results);
        }
        if (!okay) return L.error("audio blocks require frozen capture and valid range");
        L.newtable();
        for (uint32_t n = 0; n < count; ++n) {
            const auto& result = results[n];
            L.newtable(); number(L, "index", first + n);
            wide(L, "observed_cpu_cycle", result.lastPublishedCpuCycle);
            number(L, "offset", result.offset); number(L, "frames", result.frames);
            number(L, "voice_frames", result.voices); number(L, "disc_frames", result.disc);
            number(L, "flags", result.flags);
            L.rawseti(n + 1);
        }
        return 1;
    }, -1);
    L.declareFunc("events", [this](Lua L) -> int {
        if (L.gettop() != 2 || !paused(m_capture)) return L.error("audio events require paused owner, first index and count");
        const auto first = integer(L, 1, Capture::MAX_RECORDS);
        const auto count = integer(L, 2, Capture::MAX_BATCH);
        auto* results = static_cast<Capture::Event*>(L.newuser(size_t(count) * sizeof(Capture::Event)));
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.events(first, count, results);
        }
        if (!okay) return L.error("audio events require frozen capture and valid range");
        L.newtable();
        for (uint32_t n = 0; n < count; ++n) {
            const auto& result = results[n];
            L.newtable(); number(L, "id", first + n + 1); number(L, "kind", result.kind);
            wide(L, "cycle", result.cycle); wide(L, "observed_frame", result.frame); wide(L, "parent", result.parent);
            if (result.kind == uint32_t(Capture::Kind::Feed)) {
                number(L, "source_rate", result.address); number(L, "output_frames", result.value);
                number(L, "source_frames", result.width); number(L, "stereo", result.detail);
                number(L, "accepted", result.flags & 1); number(L, "cdda", (result.flags >> 1) & 1);
            } else {
                number(L, "address", result.address); number(L, "value", result.value); number(L, "width", result.width);
            }
            L.rawseti(n + 1);
        }
        return 1;
    }, -1);
    L.declareFunc("exportPCM", [this](Lua L) -> int {
        if (L.gettop() != 2 || !paused(m_capture)) return L.error("audio PCM export requires paused owner and two capture names");
        if (L.type(1) != LUA_TSTRING || L.type(2) != LUA_TSTRING) return L.error("audio PCM export names must be strings");
        size_t rawLength, wavLength;
        const auto* rawPath = lua_tolstring(L.getState(), 1, &rawLength);
        const auto* wavPath = lua_tolstring(L.getState(), 2, &wavLength);
        const std::string_view rawName(rawPath, rawLength), wavName(wavPath, wavLength);
        if (!captureName(rawName) || !captureName(wavName) || rawName == wavName) return L.error("audio PCM export requires distinct simple capture names");
        Capture::Status status;
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid && m_capture.status(status) && status.state == 2 && status.frames <= Capture::MAX_FRAMES)
                okay = m_capture.sampleRange(0, status.frames);
        }
        if (!okay) return L.error("audio PCM export requires frozen capture and valid complete range");
        if (const auto* error = exportPCM(m_capture, m_stream, rawPath, wavPath, status.frames)) return L.error(error);
        L.newtable();
        number(L, "frames", status.frames); number(L, "pcm_bytes", uint64_t(status.frames) * 8);
        number(L, "wav_bytes", uint64_t(status.frames) * 8 + 58);
        return 1;
    }, -1);
    L.declareFunc("samples", [this](Lua L) -> int {
        if (L.gettop() != 2 || !paused(m_capture)) return L.error("audio samples requires paused owner, offset and frame count");
        const auto offset = integer(L, 1, Capture::MAX_FRAMES);
        const auto frames = integer(L, 2, 65536);
        std::vector<float> samples(size_t(frames) * 2);
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.samples(offset, frames, samples.data());
        }
        if (!okay) throw std::runtime_error("audio samples requires frozen capture and valid range");
        std::string bytes(size_t(frames) * 8, '\0');
        for (size_t n = 0; n < samples.size(); ++n) {
            uint32_t bits;
            std::memcpy(&bits, &samples[n], sizeof(bits));
            for (unsigned b = 0; b < 4; ++b) bytes[n * 4 + b] = uint8_t(bits >> (b * 8));
        }
        L.push(bytes);
        return 1;
    }, -1);
    L.pop(); L.pop();
}
