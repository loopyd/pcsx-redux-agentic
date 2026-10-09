// SPDX-License-Identifier: GPL-2.0-or-later
#include "spu/sdlaudio.h"

#include <cmath>
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
void number(PCSX::Lua L, const char* key, uint32_t value) { L.push(key); L.push(lua_Number(value)); L.settable(); }
void wide(PCSX::Lua L, const char* key, uint64_t value) { L.push(key); L.push(std::to_string(value)); L.settable(); }
void string(PCSX::Lua L, const char* key, std::string_view value) { L.push(key); L.push(value); L.settable(); }
const char* const settingNames[] = {"volume", "interpolation", "reverb", "mute", "mono", "streaming",
    "null_sync", "irq_wait", "decode_irq", "scaler", "forced_irq", "voice_mute", "voice_solo", "xa"};
}

std::array<int64_t, 14> PCSX::SPU::SDLAudio::captureSettings() const {
    const auto voices = m_influences ? m_influences() : std::array<uint32_t, 2>{};
    return {m_settings.get<Volume>().value, m_settings.get<Interpolation>().value, m_settings.get<Reverb>().value,
        m_settings.get<Mute>().value, m_settings.get<Mono>().value, m_settings.get<Streaming>().value,
        m_settings.get<NullSync>().value, m_settings.get<SPUIRQWait>().value, m_settings.get<DBufIRQ>().value,
        g_emulator->settings.get<Emulator::SettingScaler>().value,
        g_emulator->settings.get<Emulator::SettingSpuIrq>().value, voices[0], voices[1],
        g_emulator->settings.get<Emulator::SettingXa>().value};
}

bool PCSX::SPU::SDLAudio::captureEnvironment(Environment& value) {
    if (!m_capture.ownerThread()) { m_capture.fail(Capture::Thread); return false; }
    const auto& backend = m_settings.get<Backend>().value;
    const auto& device = m_settings.get<Device>().value;
    if (backend.size() > 1024 || device.size() > 1024) return false;
    const auto settings = captureSettings();
    if (settings[0] < 0 || settings[0] > 3 || settings[1] < 0 || settings[1] > 3 ||
        settings[2] < 0 || settings[2] > 2 || settings[9] <= 0) return false;
    value = {settings, backend, device, m_audioInitialized, m_stream != nullptr};
    return true;
}

bool PCSX::SPU::SDLAudio::checkEnvironment(const Environment& value) {
    if (!m_capture.ownerThread()) { m_capture.fail(Capture::Thread); return false; }
    return value.settings == captureSettings() && value.backend == m_settings.get<Backend>().value &&
        value.device == m_settings.get<Device>().value && value.initialized == m_audioInitialized &&
        value.stream == (m_stream != nullptr);
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
        if (settings[0] < 0 || settings[0] > 3 || settings[1] < 0 || settings[1] > 3 ||
            settings[2] < 0 || settings[2] > 2 || settings[9] <= 0) return L.error("unsupported mixer settings");
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
            for (unsigned n = 0; n < m_captureSettings.size(); ++n) number(L, settingNames[n], m_captureSettings[n]);
            string(L, "configured_backend", m_captureBackend); string(L, "configured_device", m_captureDevice);
            string(L, "active_driver", m_captureDriver);
            L.settable();
        }
        return 1;
    }, -1);
    L.declareFunc("block", [this](Lua L) -> int {
        if (L.gettop() != 1 || !paused(m_capture)) return L.error("audio block requires paused owner and one index");
        const auto index = integer(L, 1, Capture::MAX_RECORDS - 1);
        Capture::Block result;
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.block(index, result);
        }
        if (!okay) return L.error("audio block requires frozen capture and valid index");
        L.newtable(); number(L, "index", index);
        wide(L, "observed_cpu_cycle", result.lastPublishedCpuCycle);
        number(L, "offset", result.offset); number(L, "frames", result.frames);
        number(L, "voice_frames", result.voices); number(L, "disc_frames", result.disc);
        number(L, "flags", result.flags);
        return 1;
    }, -1);
    L.declareFunc("event", [this](Lua L) -> int {
        if (L.gettop() != 1 || !paused(m_capture)) return L.error("audio event requires paused owner and one index");
        const auto index = integer(L, 1, Capture::MAX_RECORDS - 1);
        Capture::Event result;
        bool okay = false;
        {
            StreamGuard lock(m_stream);
            if (lock.valid) okay = m_capture.event(index, result);
        }
        if (!okay) return L.error("audio event requires frozen capture and valid index");
        L.newtable(); number(L, "id", index + 1); number(L, "kind", result.kind);
        wide(L, "cycle", result.cycle); wide(L, "observed_frame", result.frame); wide(L, "parent", result.parent);
        if (result.kind == uint32_t(Capture::Kind::Feed)) {
            number(L, "source_rate", result.address); number(L, "output_frames", result.value);
            number(L, "source_frames", result.width); number(L, "stereo", result.detail);
            number(L, "accepted", result.flags & 1); number(L, "cdda", (result.flags >> 1) & 1);
        } else {
            number(L, "address", result.address); number(L, "value", result.value); number(L, "width", result.width);
        }
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
