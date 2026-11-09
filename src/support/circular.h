/*

MIT License

Copyright (c) 2021 PCSX-Redux authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#pragma once

#include <memory.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace PCSX {

template <typename T, size_t BS = 1024>
class Circular {
    using ms = std::chrono::milliseconds;

  public:
    static constexpr size_t BUFFER_SIZE = BS;
    struct Admission {
        size_t queuedBefore, queuedAfter;
        uint64_t waitNanoseconds;
        bool blocked, accepted;
    };
    size_t available() {
        std::unique_lock<std::mutex> l(m_mu);
        return availableLocked();
    }
    size_t buffered() {
        std::unique_lock<std::mutex> l(m_mu);
        return bufferedLocked();
    }
    Admission enqueue(const T* data, size_t N, ms maxWait = ms{200}) {
        if (N > BUFFER_SIZE) {
            throw std::runtime_error("Trying to enqueue too much data");
        }
        std::unique_lock<std::mutex> l(m_mu);
        const auto before = bufferedLocked();
        const bool blocked = N >= availableLocked();
        using Clock = std::chrono::steady_clock;
        const auto begin = blocked ? Clock::now() : Clock::time_point{};
        bool safe = m_cv.wait_for(l, maxWait, [this, N]() -> bool { return N < availableLocked(); });
        const uint64_t elapsed = blocked
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count() : 0;
        if (safe) enqueueSafe(data, N);
        return {before, bufferedLocked(), elapsed, blocked, safe};
    }
    size_t dequeue(T* data, size_t N) {
        std::unique_lock<std::mutex> l(m_mu);
        N = std::min(N, bufferedLocked());
        dequeueSafe(data, N);

        return N;
    }

  private:
    size_t availableLocked() const { return BUFFER_SIZE - m_size; }
    size_t bufferedLocked() const { return m_size; }
    void enqueueSafe(const T* data, size_t N) {
        size_t end = m_end;
        const size_t subLen = BUFFER_SIZE - end;
        if (N > subLen) {
            enqueueSafe(data, subLen);
            enqueueSafe(data + subLen, N - subLen);
        } else {
            memcpy(m_buffer + end, data, N * sizeof(T));
            end += N;
            if (end == BUFFER_SIZE) end = 0;
            m_end = end;
            m_size += N;
        }
    }
    void dequeueSafe(T* data, size_t N) {
        size_t begin = m_begin;
        const size_t subLen = BUFFER_SIZE - begin;
        if (N > subLen) {
            dequeueSafe(data, subLen);
            dequeueSafe(data + subLen, N - subLen);
        } else {
            memcpy(data, m_buffer + begin, N * sizeof(T));
            begin += N;
            if (begin == BUFFER_SIZE) begin = 0;
            m_begin = begin;
            m_size -= N;
            m_cv.notify_one();
        }
    }

    size_t m_begin = 0, m_end = 0, m_size = 0;
    T m_buffer[BUFFER_SIZE];

    std::mutex m_mu;
    std::condition_variable m_cv;
};
}  // namespace PCSX
