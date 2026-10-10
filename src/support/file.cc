/*

MIT License

Copyright (c) 2019 PCSX-Redux authors

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

#include "support/file.h"

#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "support/slice.h"

uint8_t PCSX::BufferFile::m_internalBuffer = 0;

PCSX::BufferFile::BufferFile(void *data, size_t size) : File(RO_SEEKABLE) {
    m_data = reinterpret_cast<uint8_t *>(data);
    m_size = size;
}

PCSX::BufferFile::BufferFile(void *data, size_t size, FileOps::ReadWrite) : File(RW_SEEKABLE) {
    m_data = reinterpret_cast<uint8_t *>(malloc(size));
    if (m_data == nullptr) throw std::runtime_error("Out of memory");
    memcpy(m_data, data, size);
    m_size = m_allocSize = size;
    m_owned = true;
}

PCSX::BufferFile::BufferFile(void *data, size_t size, Acquire) : File(RW_SEEKABLE) {
    m_data = reinterpret_cast<uint8_t *>(data);
    m_size = m_allocSize = size;
    m_owned = true;
}

PCSX::BufferFile::BufferFile(void *data, size_t size, Borrow) : File(RW_SEEKABLE) {
    m_data = reinterpret_cast<uint8_t *>(data);
    m_size = size;
    m_owned = false;
}

PCSX::BufferFile::BufferFile() : File(RO_SEEKABLE) {
    m_data = &m_internalBuffer;
    m_size = 1;
}

PCSX::BufferFile::BufferFile(FileOps::ReadWrite) : File(RW_SEEKABLE) {
    m_data = nullptr;
    m_owned = true;
}

PCSX::BufferFile::BufferFile(Slice &&slice) : File(RO_SEEKABLE), m_slice(std::move(slice)) {
    m_data = const_cast<uint8_t *>(m_slice.data<uint8_t>());
    m_size = m_slice.size();
}

void PCSX::BufferFile::closeInternal() {
    if (m_owned) free(m_data);
    m_owned = true;
    m_data = nullptr;
    m_size = 0;
    m_ptrR = 0;
    m_ptrW = 0;
}

ssize_t PCSX::BufferFile::rSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrR = pos;
            break;
        case SEEK_END:
            m_ptrR = m_size - pos;
            break;
        case SEEK_CUR:
            m_ptrR += pos;
            break;
    }
    m_ptrR = std::max(std::min(m_ptrR, m_size), size_t(0));
    return m_ptrR;
}

ssize_t PCSX::BufferFile::wSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrW = pos;
            break;
        case SEEK_END:
            m_ptrW = m_size - pos;
            break;
        case SEEK_CUR:
            m_ptrW += pos;
            break;
    }
    m_ptrW = std::max(m_ptrW, size_t(0));
    return m_ptrW;
}

ssize_t PCSX::BufferFile::read(void *dest, size_t size) {
    size = std::min(m_size - m_ptrR, size);
    if (size == 0) return -1;
    memcpy(dest, m_data + m_ptrR, size);
    m_ptrR += size;
    return size;
}

ssize_t PCSX::BufferFile::write(const void *src, size_t size) {
    if (!writable()) return -1;
    size_t newSize = m_ptrW + size;
    if (newSize > m_size) {
        m_size = newSize;
    }
    if (newSize > m_allocSize) {
        static_assert((sizeof(newSize) == 4) || (sizeof(newSize) == 8));
        newSize--;
        newSize |= newSize >> 1;
        newSize |= newSize >> 2;
        newSize |= newSize >> 4;
        newSize |= newSize >> 8;
        newSize |= newSize >> 16;
        if (sizeof(newSize) == 8) newSize |= newSize >> 32;
        newSize++;
        // TODO: maybe cap the power-of-two increase..?
        m_data = reinterpret_cast<uint8_t *>(realloc(m_data, newSize));
        if (m_data == nullptr) throw std::runtime_error("Out of memory");
        m_allocSize = newSize;
    }

    memcpy(m_data + m_ptrW, src, size);
    m_ptrW += size;
    return size;
}

bool PCSX::BufferFile::eof() { return m_size == m_ptrR; }

PCSX::File *PCSX::BufferFile::dup() {
    if (m_owned && !writable()) {
        return new BufferFile(m_data, m_size);
    } else {
        return new BufferFile(m_data, m_size, FileOps::READWRITE);
    }
}

PCSX::Slice PCSX::BufferFile::borrow(size_t offset) {
    Slice ret;
    ret.borrow(m_data + offset, m_size - offset);
    return ret;
}

void PCSX::PosixFile::closeInternal() {
    if (m_handle) {
        fclose(m_handle);
        m_handle = nullptr;
    }
}

PCSX::PosixFile::PosixFile(const char *filename) : File(RO_SEEKABLE), m_filename(filename) {
    m_handle = fopen(filename, "rb");
}
PCSX::PosixFile::PosixFile(const char *filename, FileOps::Create) : File(RW_SEEKABLE), m_filename(filename) {
    m_handle = fopen(filename, "ab+");
}
PCSX::PosixFile::PosixFile(const char *filename, FileOps::Truncate) : File(RW_SEEKABLE), m_filename(filename) {
    m_handle = fopen(filename, "wb+");
}
PCSX::PosixFile::PosixFile(const char *filename, FileOps::ReadWrite) : File(RW_SEEKABLE), m_filename(filename) {
    m_handle = fopen(filename, "rb+");
}

PCSX::PosixFile::PosixFile(std::filesystem::path filename, DeferredOpen mode)
    : File(mode == DeferredOpen::Create ? RW_SEEKABLE : RO_SEEKABLE),
      m_filename(std::move(filename)), m_deferredOpen(true) {
    const auto encoded = m_filename.u8string();
    m_deferredName.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
}

bool PCSX::PosixFile::openPrepared() {
    if (!m_deferredOpen || m_handle || closedForRawAccess()) return false;
    m_deferredOpen = false;
    m_handle = fopen(m_deferredName.c_str(), writable() ? "ab+" : "rb");
    return m_handle != nullptr;
}

ssize_t PCSX::PosixFile::rSeek(ssize_t pos, int wheel) {
    if (failed()) throw std::runtime_error("Invalid file");
    ssize_t ret = 0;
    switch (wheel) {
        case SEEK_SET:
        case SEEK_END:
            ret = fseek(m_handle, pos, wheel);
            break;
        case SEEK_CUR:
            ret = fseek(m_handle, pos + m_ptrR, SEEK_SET);
            break;
    }
    if (ret < 0) throw std::runtime_error("Error seeking file...");
    m_ptrR = ftell(m_handle);
    return m_ptrR;
}

ssize_t PCSX::PosixFile::wSeek(ssize_t pos, int wheel) {
    if (failed()) throw std::runtime_error("Invalid file");
    ssize_t ret = 0;
    switch (wheel) {
        case SEEK_SET:
        case SEEK_END:
            ret = fseek(m_handle, pos, wheel);
            break;
        case SEEK_CUR:
            ret = fseek(m_handle, pos + m_ptrW, SEEK_SET);
            break;
    }
    if (ret < 0) throw std::runtime_error("Error seeking file...");
    m_ptrW = ftell(m_handle);
    return m_ptrW;
}

size_t PCSX::PosixFile::size() {
    if (failed()) throw std::runtime_error("Invalid file");
    ssize_t ret = fseek(m_handle, 0, SEEK_END);
    if (ret < 0) throw std::runtime_error("Can't seek file...");
    return ftell(m_handle);
}

ssize_t PCSX::PosixFile::read(void *dest, size_t size) {
    if (failed()) throw std::runtime_error("Invalid file");
    if (feof(m_handle)) return -1;
    ssize_t ret = fseek(m_handle, m_ptrR, SEEK_SET);
    if (ret < 0) throw std::runtime_error("Error seeking file...");
    ret = fread(dest, 1, size, m_handle);
    if (ret < 0) throw std::runtime_error("Error reading file...");
    m_ptrR += ret;
    return ret;
}

ssize_t PCSX::PosixFile::write(const void *src, size_t size) {
    if (failed()) throw std::runtime_error("Invalid file");
    ssize_t ret = fseek(m_handle, m_ptrW, SEEK_SET);
    if (ret < 0) throw std::runtime_error("Error seeking file...");
    ret = fwrite(src, 1, size, m_handle);
    if (ret < 0) throw std::runtime_error("Error writing file...");
    m_ptrW += ret;
    return ret;
}

ssize_t PCSX::SubFile::rSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrR = pos;
            break;
        case SEEK_END:
            m_ptrR = m_size - pos;
            break;
        case SEEK_CUR:
            m_ptrR += pos;
            break;
    }
    m_ptrR = std::max(std::min(m_ptrR, m_size), size_t(0));
    return m_ptrR;
}

ssize_t PCSX::SubFile::read(void *dest, size_t size) {
    ssize_t ret = readAt(dest, size, m_ptrR);
    if (ret < 0) return ret;
    m_ptrR += ret;
    if ((m_ptrR < 0) || (m_ptrR > m_size)) {
        throw std::runtime_error("SubFile pointer got out of bound - shouldn't happen");
    }
    return ret;
}

ssize_t PCSX::SubFile::readAt(void *dest, size_t size, size_t ptr) {
    ssize_t excess = size + ptr - m_size;
    if (excess > 0) {
        if (excess > size) {
            return -1;
        }
        size -= excess;
    }
    return m_file->readAt(dest, size, ptr + m_start);
}

ssize_t PCSX::SubFile::wSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrW = pos;
            break;
        case SEEK_END:
            m_ptrW = m_size - pos;
            break;
        case SEEK_CUR:
            m_ptrW += pos;
            break;
    }
    m_ptrW = std::max(std::min(m_ptrW, m_size), size_t(0));
    return m_ptrW;
}

ssize_t PCSX::SubFile::write(const void *src, size_t size) {
    ssize_t ret = writeAt(src, size, m_ptrW);
    if (ret < 0) return ret;
    m_ptrW += ret;
    if ((m_ptrW < 0) || (m_ptrW > m_size)) {
        throw std::runtime_error("SubFile write pointer got out of bound - shouldn't happen");
    }
    return ret;
}

ssize_t PCSX::SubFile::writeAt(const void *src, size_t size, size_t ptr) {
    ssize_t excess = size + ptr - m_size;
    if (excess > 0) {
        if (excess > size) {
            return -1;
        }
        size -= excess;
    }
    return m_file->writeAt(src, size, ptr + m_start);
}

ssize_t PCSX::Fifo::read(void *dest_, size_t size) {
    if (size == 0) return 0;
    uint8_t *dest = static_cast<uint8_t *>(dest_);
    ssize_t ret = 0;
    while (size != 0) {
        if (m_slices.empty()) {
            return ret == 0 ? -1 : ret;
        }
        Slice &slice = m_slices.front();
        auto tocopy = std::min(size, slice.size() - m_ptrR);
        memcpy(dest + ret, slice.data<uint8_t>() + m_ptrR, tocopy);
        size -= tocopy;
        ret += tocopy;
        m_size -= tocopy;
        m_ptrR += tocopy;
        if (slice.size() == m_ptrR) {
            m_slices.pop();
            m_ptrR = 0;
        }
    }

    return ret;
}

PCSX::File::RawSnapshotStatus PCSX::File::resolveRawSelection(RawSelection& selection) noexcept {
    File* current = this;
    size_t previousStart = 0, previousLength = 0;
    bool previousWindow = false;
    selection = {};
    for (;;) {
        if (current->closedForRawAccess()) return RawSnapshotStatus::Unsupported;
        auto view = current->rawView();
        if (view.kind == RawKind::Unsupported) return RawSnapshotStatus::Unsupported;
        if (view.kind == RawKind::Descriptor) {
            struct stat metadata;
            int status;
            do { status = fstat(view.descriptor, &metadata); } while (status < 0 && errno == EINTR);
            if (status < 0) return errno == ENOMEM ? RawSnapshotStatus::AllocationFailed : RawSnapshotStatus::Invalid;
            if (!S_ISREG(metadata.st_mode)) return RawSnapshotStatus::Unsupported;
            if (metadata.st_size < 0 || uintmax_t(metadata.st_size) > std::numeric_limits<size_t>::max())
                return RawSnapshotStatus::Invalid;
            view.extent = static_cast<size_t>(metadata.st_size);
        }
        if (previousWindow && (previousStart > view.extent || previousLength > view.extent - previousStart))
            return RawSnapshotStatus::Unsupported;
        if (view.kind != RawKind::Window) {
            selection.leaf = current;
            selection.view = view;
            if (!selection.fixed) selection.length = view.extent;
            if (selection.start > view.extent || selection.length > view.extent - selection.start)
                return RawSnapshotStatus::Unsupported;
            return RawSnapshotStatus::Ok;
        }
        if (!view.parent || view.start > std::numeric_limits<size_t>::max() - selection.start)
            return RawSnapshotStatus::Unsupported;
        if (!selection.fixed) {
            selection.fixed = true;
            selection.length = view.extent;
        }
        selection.start += view.start;
        previousStart = view.start;
        previousLength = view.extent;
        previousWindow = true;
        current = view.parent;
    }
}

PCSX::File::RawSnapshotStatus PCSX::File::stateReadExtent(size_t& bytes) noexcept {
    RawSelection selection;
    const auto status = resolveRawSelection(selection);
    if (status == RawSnapshotStatus::Ok) bytes = selection.length;
    return status;
}

bool PCSX::File::stateReadAvailable() noexcept {
    File* current = this;
    size_t start = 0;
    for (;;) {
        if (current->closedForRawAccess()) return false;
        const auto view = current->rawView();
        if (view.kind == RawKind::Unsupported) return false;
        if (view.kind != RawKind::Window) return true;
        if (!view.parent || view.start > std::numeric_limits<size_t>::max() - start) return false;
        start += view.start;
        current = view.parent;
    }
}

PCSX::File::RawSnapshotStatus PCSX::File::snapshotRawOwned(size_t maximum, std::string& output) noexcept {
    output.clear();
    try {
        IO<File> owner(this);
        RawSelection selection;
        auto status = resolveRawSelection(selection);
        if (status != RawSnapshotStatus::Ok) return status;
        if (maximum == std::numeric_limits<size_t>::max()) return RawSnapshotStatus::Invalid;
        const size_t budget = maximum + 1;
        std::string bytes;
        if (selection.view.kind == RawKind::Buffer) {
            if (selection.length > maximum) return RawSnapshotStatus::TooLarge;
            if (selection.length) bytes.assign(reinterpret_cast<const char*>(selection.view.data + selection.start), selection.length);
        } else {
            constexpr size_t chunk = 65536;
            const auto largestOffset = uintmax_t(std::numeric_limits<off_t>::max());
            if (uintmax_t(selection.start) > largestOffset) return RawSnapshotStatus::Invalid;
            if (selection.fixed && selection.length > maximum) return RawSnapshotStatus::TooLarge;
            while (!selection.fixed || bytes.size() < selection.length) {
                if (bytes.size() == budget) return RawSnapshotStatus::TooLarge;
                size_t count = std::min(chunk, budget - bytes.size());
                if (selection.fixed) count = std::min(count, selection.length - bytes.size());
                if (bytes.size() > std::numeric_limits<size_t>::max() - selection.start)
                    return RawSnapshotStatus::Invalid;
                const size_t offset = selection.start + bytes.size();
                if (uintmax_t(offset) > largestOffset || uintmax_t(count) > largestOffset - offset ||
                    count > size_t(std::numeric_limits<ssize_t>::max())) return RawSnapshotStatus::Invalid;
                const size_t received = bytes.size();
                bytes.resize(received + count);
                ssize_t readCount;
                do {
                    readCount = pread(selection.view.descriptor, bytes.data() + received, count, static_cast<off_t>(offset));
                } while (readCount < 0 && errno == EINTR);
                if (readCount < 0) return errno == ENOMEM ? RawSnapshotStatus::AllocationFailed : RawSnapshotStatus::Invalid;
                if (size_t(readCount) > count) return RawSnapshotStatus::Invalid;
                bytes.resize(received + size_t(readCount));
                if (!readCount) {
                    if (selection.fixed && bytes.size() != selection.length) return RawSnapshotStatus::Invalid;
                    break;
                }
                if (bytes.size() > maximum) return RawSnapshotStatus::TooLarge;
            }
        }
        RawSelection after;
        status = resolveRawSelection(after);
        if (status != RawSnapshotStatus::Ok) return status;
        if (after.leaf != selection.leaf || after.view.kind != selection.view.kind ||
            after.view.data != selection.view.data || after.view.descriptor != selection.view.descriptor ||
            after.start != selection.start || after.fixed != selection.fixed ||
            (selection.fixed && after.length != selection.length) ||
            (selection.view.kind == RawKind::Buffer && after.length != selection.length))
            return RawSnapshotStatus::Unsupported;
        output.swap(bytes);
        return RawSnapshotStatus::Ok;
    } catch (const std::bad_alloc&) {
        return RawSnapshotStatus::AllocationFailed;
    } catch (...) {
        return RawSnapshotStatus::Invalid;
    }
}
