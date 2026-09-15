#include "album_caps.hpp"

#include "defines.hpp"
#include "log.hpp"

#include <algorithm>

namespace sphaira::caps {
namespace {

// a 320x180 jpeg is nowhere near this, but the service is not asked for the
// size beforehand, so the buffer has to be able to take whatever it gives.
constexpr u64 THUMBNAIL_BUFFER_SIZE = 1024 * 256;

Mutex g_mutex{};
u32 g_ref_count{};

} // namespace

Result Init() {
    SCOPED_MUTEX(std::addressof(g_mutex));

    if (g_ref_count) {
        g_ref_count++;
        R_SUCCEED();
    }

    R_TRY(capsaInitialize());

    g_ref_count++;
    R_SUCCEED();
}

void Exit() {
    SCOPED_MUTEX(std::addressof(g_mutex));

    if (!g_ref_count) {
        return;
    }

    g_ref_count--;
    if (!g_ref_count) {
        capsaExit();
    }
}

Result GetEntries(CapsAlbumStorage storage, std::vector<CapsAlbumEntry>& out) {
    SCOPED_MUTEX(std::addressof(g_mutex));

    out.clear();

    u64 count{};
    R_TRY(capsaGetAlbumFileCount(storage, std::addressof(count)));

    if (!count) {
        R_SUCCEED();
    }

    out.resize(count);

    u64 total{};
    R_TRY(capsaGetAlbumFileList(storage, std::addressof(total), out.data(), out.size()));

    out.resize(std::min(total, count));
    R_SUCCEED();
}

Result LoadThumbnail(const CapsAlbumFileId& file_id, std::vector<u8>& out) {
    SCOPED_MUTEX(std::addressof(g_mutex));

    out.resize(THUMBNAIL_BUFFER_SIZE);

    u64 size{};
    const auto rc = capsaLoadAlbumFileThumbnail(std::addressof(file_id), std::addressof(size), out.data(), out.size());
    if (R_FAILED(rc)) {
        out.clear();
        return rc;
    }

    out.resize(std::min(size, (u64)out.size()));
    R_SUCCEED();
}

Result LoadFile(const CapsAlbumFileId& file_id, std::vector<u8>& out) {
    SCOPED_MUTEX(std::addressof(g_mutex));

    u64 file_size{};
    R_TRY(capsaGetAlbumFileSize(std::addressof(file_id), std::addressof(file_size)));

    out.resize(file_size);

    u64 size{};
    const auto rc = capsaLoadAlbumFile(std::addressof(file_id), std::addressof(size), out.data(), out.size());
    if (R_FAILED(rc)) {
        out.clear();
        return rc;
    }

    out.resize(std::min(size, file_size));
    R_SUCCEED();
}

Result DeleteFile(const CapsAlbumFileId& file_id) {
    SCOPED_MUTEX(std::addressof(g_mutex));
    return capsaDeleteAlbumFile(std::addressof(file_id));
}

MovieStream::~MovieStream() {
    Close();
}

Result MovieStream::Open(const CapsAlbumFileId& file_id) {
    R_UNLESS(hosversionAtLeast(4,0,0), MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer));

    Close();

    SCOPED_MUTEX(std::addressof(g_mutex));
    R_TRY(capsaOpenAlbumMovieStream(std::addressof(m_stream), std::addressof(file_id)));

    m_open = true;
    R_SUCCEED();
}

void MovieStream::Close() {
    if (!m_open) {
        return;
    }

    SCOPED_MUTEX(std::addressof(g_mutex));
    capsaCloseAlbumMovieStream(m_stream);

    m_stream = 0;
    m_open = false;
}

Result MovieStream::GetSize(u64* out) {
    R_UNLESS(m_open, MAKERESULT(Module_Libnx, LibnxError_NotInitialized));

    SCOPED_MUTEX(std::addressof(g_mutex));
    return capsaGetAlbumMovieStreamSize(m_stream, out);
}

Result MovieStream::Read(s64 offset, void* buf, u64 size, u64* out) {
    R_UNLESS(m_open, MAKERESULT(Module_Libnx, LibnxError_NotInitialized));

    SCOPED_MUTEX(std::addressof(g_mutex));
    return capsaReadMovieDataFromAlbumMovieReadStream(m_stream, offset, buf, size, out);
}

} // namespace sphaira::caps
