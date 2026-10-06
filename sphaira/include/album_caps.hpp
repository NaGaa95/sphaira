#pragma once

#include <switch.h>
#include <vector>

// thin wrapper over caps:a (the album accessor the console uses for its own
// album). the ui thread and the album server both talk to it, and a service
// session cannot be used from two threads at once, so every call in here is
// serialised.
namespace sphaira::caps {

// ref counted.
Result Init();
void Exit();

// lists every capture on the given storage.
Result GetEntries(CapsAlbumStorage storage, std::vector<CapsAlbumEntry>& out);

// 320x180 jpeg, as used by the console's own album grid.
Result LoadThumbnail(const CapsAlbumFileId& file_id, std::vector<u8>& out);

// the capture itself, jpeg or mp4. prefer MovieStream for clips, they are
// far too big to want in memory.
Result LoadFile(const CapsAlbumFileId& file_id, std::vector<u8>& out);

Result DeleteFile(const CapsAlbumFileId& file_id);

// reads a clip in chunks, so that serving one costs a buffer rather than the
// whole file. needs [4.0.0+], Open() fails below that.
struct MovieStream final {
    ~MovieStream();

    MovieStream() = default;
    MovieStream(const MovieStream&) = delete;
    void operator=(const MovieStream&) = delete;

    Result Open(const CapsAlbumFileId& file_id);
    void Close();

    Result GetSize(u64* out);
    // offset and size must both be multiples of ALIGNMENT.
    Result Read(s64 offset, void* buf, u64 size, u64* out);

    auto IsOpen() const -> bool {
        return m_open;
    }

    static constexpr u64 ALIGNMENT = 0x40000;

private:
    u64 m_stream{};
    bool m_open{};
};

} // namespace sphaira::caps
