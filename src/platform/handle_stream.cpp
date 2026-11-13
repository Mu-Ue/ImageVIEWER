#include "platform/handle_stream.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr ULONG kBufferBytes = 64 << 10;

// Reads and writes at the stream's own position, so a seek is never a call into the kernel, and reads go through
// a buffer: decoders read a few KB at a time, and some seek before every read.
class HandleStream final : public IStream {
public:
    explicit HandleStream(HANDLE file) : file_(file) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) || iid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = ULONG(InterlockedDecrement(&refs_));
        if (refs == 0) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* read) override {
        BYTE* to = static_cast<BYTE*>(data);
        ULONG done = 0;
        HRESULT hr = S_OK;
        while (done < size) {
            if (pos_ >= bufferAt_ && pos_ - bufferAt_ < bufferLength_) {
                const ULONG n = ULONG(std::min<ULONGLONG>(size - done, bufferAt_ + bufferLength_ - pos_));
                memcpy(to + done, buffer_.get() + (pos_ - bufferAt_), n);
                pos_ += n;
                done += n;
                continue;
            }
            const ULONG rest = size - done;
            if (!buffer_ && rest < kBufferBytes) buffer_.reset(new (std::nothrow) BYTE[kBufferBytes]);
            ULONG n = 0;
            if (rest >= kBufferBytes || !buffer_) {  // too large to be worth buffering
                hr = ReadAt(pos_, to + done, rest, n);
                pos_ += n;
                done += n;
                break;
            }
            // Below the buffer, as when rows stored bottom-up are read top-down, fill the buffer with what lies
            // further below; otherwise with what follows.
            const ULONGLONG end = pos_ + rest;
            const ULONGLONG at = pos_ < bufferAt_ && bufferLength_ ? (end > kBufferBytes ? end - kBufferBytes : 0) : pos_;
            bufferLength_ = 0;
            hr = ReadAt(at, buffer_.get(), kBufferBytes, n);
            bufferAt_ = at;
            bufferLength_ = n;
            if (FAILED(hr) || at + n <= pos_) break;  // the end of the file
        }
        if (read) *read = done;
        return FAILED(hr) ? hr : done < size ? S_FALSE : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* written) override {
        bufferLength_ = 0;
        OVERLAPPED at = At(pos_);
        DWORD n = 0;
        const BOOL ok = WriteFile(file_, data, size, &n, &at);
        pos_ += n;
        if (written) *written = n;
        return !ok ? HRESULT_FROM_WIN32(GetLastError()) : n < size ? STG_E_MEDIUMFULL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override {
        LARGE_INTEGER base{};  // as SetFilePointerEx would
        if (origin == STREAM_SEEK_CUR) {
            base.QuadPart = LONGLONG(pos_);
        } else if (origin == STREAM_SEEK_END) {
            if (!GetFileSizeEx(file_, &base)) return HRESULT_FROM_WIN32(GetLastError());
        } else if (origin != STREAM_SEEK_SET) {
            return STG_E_INVALIDFUNCTION;
        }
        if (move.QuadPart < -base.QuadPart) return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
        if (move.QuadPart > LLONG_MAX - base.QuadPart) return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
        pos_ = ULONGLONG(base.QuadPart + move.QuadPart);
        if (position) position->QuadPart = pos_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        bufferLength_ = 0;
        FILE_END_OF_FILE_INFO end{.EndOfFile = {.QuadPart = LONGLONG(size.QuadPart)}};
        return SetFileInformationByHandle(file_, FileEndOfFileInfo, &end, sizeof(end))
                   ? S_OK
                   : HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        LARGE_INTEGER size;
        if (!GetFileSizeEx(file_, &size)) return HRESULT_FROM_WIN32(GetLastError());
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = ULONGLONG(size.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    ~HandleStream() { CloseHandle(file_); }

    static OVERLAPPED At(ULONGLONG offset) {
        OVERLAPPED at{};
        at.Offset = DWORD(offset);
        at.OffsetHigh = DWORD(offset >> 32);
        return at;
    }

    // Reads up to size bytes at offset; fewer, with S_OK, at the end of the file.
    HRESULT ReadAt(ULONGLONG offset, void* data, ULONG size, ULONG& read) {
        OVERLAPPED at = At(offset);
        DWORD n = 0;
        const BOOL ok = ReadFile(file_, data, size, &n, &at);
        read = ok ? n : 0;
        return ok || GetLastError() == ERROR_HANDLE_EOF ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    HANDLE file_;
    LONG refs_ = 1;
    ULONGLONG pos_ = 0;
    std::unique_ptr<BYTE[]> buffer_;
    ULONGLONG bufferAt_ = 0;  // the file offset of the buffer's bytes
    ULONG bufferLength_ = 0;
};

}  // namespace

HRESULT CreateHandleStream(HANDLE file, Microsoft::WRL::ComPtr<IStream>& out) {
    IStream* stream = new (std::nothrow) HandleStream(file);
    if (!stream) return E_OUTOFMEMORY;
    out.Attach(stream);
    return S_OK;
}
