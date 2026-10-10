// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/journal.h"
#include <optional>

namespace PCSX { class GPU; }

namespace PCSX::History {

enum class Kind : uint32_t {
    RegisterAttempt = 1, RegisterResult, DmaStart, DmaData, DmaHeader,
    DmaComplete, DmaIRQ, DmaDeferred, DmaInitial, Schedule, Dispatch,
    IRQAssert, IRQClear, IRQAccept, TimerBefore, TimerAfter, TimerRead,
    TimerWrite, TimerGate, Discontinuity, DmaEnd, TimerRate, TimerInitial,
    ScheduleInitial, IRQInitial, DmaDevice, DmaBuffer, DmaCancel, DmaPeek,
    GpuContext, GpuEnvironment, GpuInput, GpuResult,
    CdContext, CdPort, CdCommand, CdSchedule, CdResponse, CdIRQ,
    CdMedia, CdBuffer, CdData, CdAudio, CdStream, CdTransition, CdPredicate, CdImage,
};

void initialize();
bool active();
bool ownerThread();
enum class StateBoundary { Unavailable, Recording, Unsettled, Ready };
StateBoundary stateBoundary() noexcept;
bool begin(uint32_t events, uint32_t bytes);
bool beginCD(uint32_t events, uint32_t bytes, bool audio);
bool beginCDV3(uint32_t events, uint32_t bytes, uint32_t workerEvents, uint32_t flags);
bool stop();
bool clear();
bool status(Status* out);
bool records(uint32_t first, uint32_t count, Record* out);
bool bytes(uint32_t offset, uint32_t length, void* out);
void fail(Failure reason);
void emit(Kind kind, uint32_t device, std::array<uint32_t, 8> data = {},
          uint64_t related = 0, const void* payload = nullptr, uint32_t length = 0,
          uint64_t request = UINT64_MAX);
void start(uint32_t channel, uint32_t original, uint32_t normalized, uint32_t bcr,
           uint32_t chcr, uint32_t previous, uint32_t enabled);
void scalar(uint32_t channel, uint32_t address, uint32_t destination,
            uint32_t value, uint32_t width, uint32_t direction);
void span(uint32_t channel, uint32_t address, const void* data, uint32_t length, uint32_t direction,
          uint64_t related = 0);
uint64_t input();
void retain();
void inputScalar(uint32_t address, uint16_t value, bool peek = false);
void defer(uint32_t address, uint32_t bcr, uint32_t chcr);
uint64_t pending();
void release();
uint64_t buffer();
void buffer(const void* data, uint32_t length);
void cancel();
uint64_t dmaRequest(uint32_t channel);
void timerRead(uint32_t index, uint32_t selector, uint32_t value, uint32_t after = 0);

enum class GpuSource : uint32_t { GP0, GP1, Direct, Chain, ReadDMA, ReadGP0, HostGP0, HostGP1, HostRead, HostReplay };
class Gpu {
  public:
    Gpu(GPU& gpu, GpuSource source, uint32_t origin, uint32_t words, uint32_t value = 0);
    ~Gpu();
    void result(uint32_t value) { m_value = value; }
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;
  private:
    void emit(Kind kind);
    GPU* m_gpu = nullptr;
    GpuSource m_source;
    uint32_t m_origin, m_words, m_value;
    uint64_t m_id = 0, m_epoch = 0, m_request = 0;
    bool m_scope = false;
};

class Register {
  public:
    Register(uint32_t address, uint32_t value, uint32_t width, bool read = false);
    ~Register();
  private:
    uint32_t m_address, m_value, m_width;
    bool m_observed = false;
    bool m_read;
    std::array<uint32_t, 2> m_previous{};
};

class Completion {
  public:
    explicit Completion(uint32_t channel);
    ~Completion();
  private:
    uint32_t m_channel;
    bool m_observed = false;
};

class Owner {
  public:
    Owner(uint32_t channel, uint64_t request);
    ~Owner();
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
  private:
    uint32_t m_channel = 0;
    uint64_t m_previous = 0, m_epoch = 0;
    bool m_prior = false, m_observed = false;
};
class Dispatch {
  public:
    Dispatch(uint32_t interrupt, uint64_t target);
  private:
    std::optional<Owner> m_owner;
};

}
