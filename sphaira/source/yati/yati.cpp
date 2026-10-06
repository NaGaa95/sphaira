#include "yati/yati.hpp"
#include "yati/source/file.hpp"
#include "yati/source/stream_file.hpp"
#include "yati/container/nsp.hpp"
#include "yati/container/xci.hpp"
#include "msp.hpp"

#include "yati/nx/ncz.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"
#include "yati/nx/ns.hpp"
#include "yati/nx/es.hpp"
#include "yati/nx/keys.hpp"
#include "yati/nx/crypto.hpp"

#include "utils/utils.hpp"
#include "utils/thread.hpp"

#include "ui/progress_box.hpp"
#include "ui/menus/game_menu.hpp"
#include "ui/menus/ownfoil.hpp"

#include "app.hpp"
#include "i18n.hpp"
#include "log.hpp"

#include <zstd.h>
#include <minIni.h>
#include <algorithm>
#include <atomic>

namespace sphaira::yati {
namespace {

constexpr NcmStorageId NCM_STORAGE_IDS[]{
    NcmStorageId_BuiltInUser,
    NcmStorageId_SdCard,
};

constexpr u32 KEYGEN_LIMIT = 0x20;

struct NcaCollection : container::CollectionEntry {
    nca::Header header{};
    // NcmContentType
    u8 type{};
    NcmContentId content_id{};
    NcmPlaceHolderId placeholder_id{};
    // new hash of the nca..
    u8 hash[SHA256_HASH_SIZE]{};
    // set true if nca has been modified.
    bool modified{};
    // set if the nca was not installed.
    bool skipped{};
};

struct CnmtCollection : NcaCollection {
    // list of all nca's the cnmt depends on
    std::vector<NcaCollection> ncas{};
    // only set if any of the nca's depend on a ticket / cert.
    // if set, the ticket / cert will be installed once all nca's have installed.
    std::vector<FsRightsId> rights_id{};

    NcmContentMetaHeader meta_header{};
    NcmContentMetaKey key{};
    NcmContentInfo content_info{};
    std::vector<u8> extended_header{};
    std::vector<NcmPackagedContentInfo> infos{};
};

struct TikCollection {
    // raw data of the ticket / cert.
    std::vector<u8> ticket{};
    std::vector<u8> cert{};
    // set via the name of the ticket.
    FsRightsId rights_id{};
    // retrieved via the master key set in nca.
    u8 key_gen{};
    // set if ticket is required by an nca.
    bool required{};
    // set if ticket has already been patched.
    bool patched{};
};

struct Yati;

const u64 INFLATE_BUFFER_MAX = 1024*1024*4;

struct ThreadBuffer {
    ThreadBuffer() {
        buf.reserve(INFLATE_BUFFER_MAX);
    }

    std::vector<u8> buf;
    s64 off;
};

template<std::size_t Size>
struct RingBuf {
private:
    ThreadBuffer buf[Size]{};
    unsigned r_index{};
    unsigned w_index{};

    static_assert((sizeof(RingBuf::buf) & (sizeof(RingBuf::buf) - 1)) == 0, "Must be power of 2!");

public:
    void ringbuf_reset() {
        this->r_index = this->w_index;
    }

    unsigned ringbuf_capacity() const {
        return sizeof(this->buf) / sizeof(this->buf[0]);
    }

    unsigned ringbuf_size() const {
        return (this->w_index - this->r_index) % (ringbuf_capacity() * 2U);
    }

    unsigned ringbuf_free() const {
        return ringbuf_capacity() - ringbuf_size();
    }

    void ringbuf_push(std::vector<u8>& buf_in, s64 off_in) {
        auto& value = this->buf[this->w_index % ringbuf_capacity()];
        value.off = off_in;
        std::swap(value.buf, buf_in);

        this->w_index = (this->w_index + 1U) % (ringbuf_capacity() * 2U);
    }

    void ringbuf_pop(std::vector<u8>& buf_out, s64& off_out) {
        auto& value = this->buf[this->r_index % ringbuf_capacity()];
        off_out = value.off;
        std::swap(value.buf, buf_out);

        this->r_index = (this->r_index + 1U) % (ringbuf_capacity() * 2U);
    }
};

struct ThreadData {
    ThreadData(Yati* _yati, std::span<TikCollection> _tik, NcaCollection* _nca)
    : yati{_yati}, tik{_tik}, nca{_nca} {
        mutexInit(std::addressof(read_mutex));
        mutexInit(std::addressof(write_mutex));

        condvarInit(std::addressof(can_read));
        condvarInit(std::addressof(can_decompress));
        condvarInit(std::addressof(can_decompress_write));
        condvarInit(std::addressof(can_write));

        ueventCreate(&m_uevent_done, false);
        ueventCreate(&m_uevent_progres, true);

        sha256ContextCreate(&sha256);
        // this will be updated with the actual size from nca header.
        write_size = nca->size;

        // reduce buffer size to preve
        if (App::IsFileBaseEmummc()) {
            read_buffer_size = 1024 * 512;
        } else {
            read_buffer_size = 1024*1024*4;
        }

        max_buffer_size = std::max(read_buffer_size, INFLATE_BUFFER_MAX);
    }

    auto GetResults() volatile -> Result;
    void WakeAllThreads();

    auto IsAnyRunning() volatile const -> bool {
        return read_running || decompress_running || write_running;
    }

    auto GetWriteOffset() volatile const -> s64 {
        return write_offset;
    }

    auto GetWriteSize() volatile const -> s64 {
        return write_size;
    }

    auto GetDoneEvent() {
        return &m_uevent_done;
    }

    auto GetProgressEvent() {
        return &m_uevent_progres;
    }

    void SetReadResult(Result result) {
        read_result = result;

        // wake up decompress thread as it may be waiting on data that never comes.
        condvarWakeOne(std::addressof(can_decompress));

        if (R_FAILED(result)) {
            ueventSignal(GetDoneEvent());
        }
    }

    void SetDecompressResult(Result result) {
        decompress_result = result;

        // wake up write thread as it may be waiting on data that never comes.
        condvarWakeOne(std::addressof(can_write));

        if (R_FAILED(result)) {
            ueventSignal(GetDoneEvent());
        }
    }

    void SetWriteResult(Result result) {
        write_result = result;

        // wake up decompress thread as it may be waiting on data that never comes.
        condvarWakeOne(std::addressof(can_decompress_write));

        ueventSignal(GetDoneEvent());
    }

    Result Read(void* buf, s64 size, u64* bytes_read);

    Result SetDecompressBuf(std::vector<u8>& buf, s64 off, s64 size) {
        buf.resize(size);

        mutexLock(std::addressof(read_mutex));
        if (!read_buffers.ringbuf_free()) {
            if (!write_running) {
                R_SUCCEED();
            }
            R_TRY(condvarWait(std::addressof(can_read), std::addressof(read_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(read_mutex)));
        R_TRY(GetResults());
        read_buffers.ringbuf_push(buf, off);
        return condvarWakeOne(std::addressof(can_decompress));
    }

    Result GetDecompressBuf(std::vector<u8>& buf_out, s64& off_out) {
        mutexLock(std::addressof(read_mutex));
        if (!read_buffers.ringbuf_size()) {
            if (!read_running) {
                buf_out.resize(0);
                R_SUCCEED();
            }
            R_TRY(condvarWait(std::addressof(can_decompress), std::addressof(read_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(read_mutex)));
        R_TRY(GetResults());
        read_buffers.ringbuf_pop(buf_out, off_out);
        return condvarWakeOne(std::addressof(can_read));
    }

    Result SetWriteBuf(std::vector<u8>& buf, s64 size, bool skip_verify) {
        buf.resize(size);
        if (!skip_verify) {
            sha256ContextUpdate(std::addressof(sha256), buf.data(), buf.size());
        }

        mutexLock(std::addressof(write_mutex));
        if (!write_buffers.ringbuf_free()) {
            if (!decompress_running) {
                R_SUCCEED();
            }
            R_TRY(condvarWait(std::addressof(can_decompress_write), std::addressof(write_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(write_mutex)));
        R_TRY(GetResults());
        write_buffers.ringbuf_push(buf, 0);
        return condvarWakeOne(std::addressof(can_write));
    }

    Result GetWriteBuf(std::vector<u8>& buf_out, s64& off_out) {
        mutexLock(std::addressof(write_mutex));
        if (!write_buffers.ringbuf_size()) {
            if (!decompress_running) {
                buf_out.resize(0);
                R_SUCCEED();
            }
            R_TRY(condvarWait(std::addressof(can_write), std::addressof(write_mutex)));
        }

        ON_SCOPE_EXIT(mutexUnlock(std::addressof(write_mutex)));
        R_TRY(GetResults());
        write_buffers.ringbuf_pop(buf_out, off_out);
        return condvarWakeOne(std::addressof(can_decompress_write));
    }

    // these need to be copied
    Yati* yati{};
    std::span<TikCollection> tik{};
    NcaCollection* nca{};

    // these need to be created
    Mutex read_mutex{};
    Mutex write_mutex{};

    CondVar can_read{};
    CondVar can_decompress{};
    CondVar can_decompress_write{};
    CondVar can_write{};

    UEvent m_uevent_done{};
    UEvent m_uevent_progres{};

    RingBuf<4> read_buffers{};
    RingBuf<4> write_buffers{};

    ncz::BlockHeader ncz_block_header{};
    std::vector<ncz::Section> ncz_sections{};
    std::vector<ncz::BlockInfo> ncz_blocks{};

    Sha256Context sha256{};

    u64 read_buffer_size{};
    u64 max_buffer_size{};

    // these are shared between threads
    std::atomic<s64> read_offset{};
    std::atomic<s64> decompress_offset{};
    std::atomic<s64> write_offset{};
    std::atomic<s64> write_size{};

    std::atomic<Result> read_result{};
    std::atomic<Result> decompress_result{};
    std::atomic<Result> write_result{};

    std::atomic_bool read_running{true};
    std::atomic_bool decompress_running{true};
    std::atomic_bool write_running{true};
};

struct Yati {
    Yati(ui::ProgressBox*, source::Base*);
    ~Yati();

    Result Setup(const ConfigOverride& override);
    Result InstallNca(std::span<TikCollection> tickets, NcaCollection& nca);
    Result InstallNcaInternal(std::span<TikCollection> tickets, NcaCollection& nca);
    Result InstallCnmtNca(std::span<TikCollection> tickets, CnmtCollection& cnmt, const container::Collections& collections);

    Result readFuncInternal(ThreadData* t);
    Result decompressFuncInternal(ThreadData* t);
    Result writeFuncInternal(ThreadData* t);

    Result ParseTicketsIntoCollection(std::vector<TikCollection>& tickets, const container::Collections& collections, bool read_data);
    Result GetLatestVersion(const CnmtCollection& cnmt, u32& version_out, bool& skip);
    Result ShouldSkip(const CnmtCollection& cnmt, bool& skip);
    Result ImportTickets(std::span<TikCollection> tickets);
    Result RemoveInstalledNcas(const CnmtCollection& cnmt);
    Result RegisterNcasAndPushRecord(const CnmtCollection& cnmt, u32 latest_version_num);


// private:
    ui::ProgressBox* pbox{};
    source::Base* source{};

    // for all content storages
    NcmContentStorage ncm_cs[2]{};
    NcmContentMetaDatabase ncm_db[2]{};
    // these point to the above struct
    NcmContentStorage cs{};
    NcmContentMetaDatabase db{};
    NcmStorageId storage_id{};

    std::unique_ptr<container::Base> container{};
    Config config{};
    keys::Keys keys{};

    // tracks what Setup() actually managed to acquire. these services are
    // refcounted process wide and other parts of sphaira (the game menu, the
    // game:/ devoptab, ...) hold them at the same time, so releasing one that
    // we never acquired would close the session out from under them.
    bool spl_crypto_init{};
    bool ns_init{};
    bool es_init{};
};

auto ThreadData::GetResults() volatile -> Result {
    R_TRY(yati->pbox->ShouldExitResult());
    R_TRY(read_result.load());
    R_TRY(decompress_result.load());
    R_TRY(write_result.load());
    R_SUCCEED();
}

void ThreadData::WakeAllThreads() {
    condvarWakeAll(std::addressof(can_read));
    condvarWakeAll(std::addressof(can_decompress));
    condvarWakeAll(std::addressof(can_decompress_write));
    condvarWakeAll(std::addressof(can_write));

    mutexUnlock(std::addressof(read_mutex));
    mutexUnlock(std::addressof(write_mutex));
}

Result ThreadData::Read(void* buf, s64 size, u64* bytes_read) {
    size = std::min<s64>(size, nca->size - read_offset);
    const auto rc = yati->source->Read(buf, nca->offset + read_offset, size, bytes_read);
    R_TRY(rc);

    R_UNLESS(size == *bytes_read, Result_YatiInvalidNcaReadSize);
    read_offset += *bytes_read;
    return rc;
}

auto GetTicketCollection(const nca::Header& header, std::span<TikCollection> tik) -> TikCollection* {
    TikCollection* ticket{};

    if (es::IsRightsIdValid(header.rights_id)) {
        auto it = std::ranges::find_if(tik, [&header](auto& e){
            return !std::memcmp(&header.rights_id, &e.rights_id, sizeof(e.rights_id));
        });

        if (it != tik.end()) {
            it->required = true;
            it->key_gen = header.GetKeyGeneration();
            ticket = &(*it);
        }
    }

    return ticket;
}

Result HasRequiredTicket(const nca::Header& header, TikCollection* ticket) {
    if (es::IsRightsIdValid(header.rights_id)) {
        log_write("looking for ticket %s\n", utils::hexIdToStr(header.rights_id).str);
        R_UNLESS(ticket, Result_YatiTicketNotFound);
        log_write("ticket found\n");
    }
    R_SUCCEED();
}

Result HasRequiredTicket(const nca::Header& header, std::span<TikCollection> tik) {
    auto ticket = GetTicketCollection(header, tik);
    return HasRequiredTicket(header, ticket);
}

// read thread reads all data from the source, it also handles
// parsing ncz headers, sections and reading ncz blocks
Result Yati::readFuncInternal(ThreadData* t) {
    ON_SCOPE_EXIT( t->read_running = false; );

    // the main buffer which data is read into.
    std::vector<u8> buf;
    // workaround ncz block reading ahead. if block isn't found, we usually
    // would seek back to the offset, however this is not possible in stream
    // mode, so we instead store the data to the temp buffer and pre-pend it.
    std::vector<u8> temp_buf;
    buf.reserve(t->max_buffer_size);
    temp_buf.reserve(t->max_buffer_size);

    while (t->read_offset < t->nca->size && R_SUCCEEDED(t->GetResults())) {
        const auto buffer_offset = t->read_offset.load();

        // read more data
        s64 read_size = t->read_buffer_size;
        if (!t->read_offset) {
            read_size = NCZ_SECTION_OFFSET;
        }

        s64 buf_offset = 0;
        if (!temp_buf.empty()) {
            buf = temp_buf;
            read_size -= temp_buf.size();
            buf_offset = temp_buf.size();
            temp_buf.clear();
        }

        u64 bytes_read{};
        buf.resize(buf_offset + read_size);
        R_TRY(t->Read(buf.data() + buf_offset, read_size, std::addressof(bytes_read)));
        auto buf_size = buf_offset + bytes_read;
        if (!bytes_read) {
            break;
        }

        // read enough bytes for ncz, check magic
        if (t->read_offset == NCZ_SECTION_OFFSET) {
            // check for ncz section header.
            ncz::Header header{};
            std::memcpy(std::addressof(header), buf.data() + 0x4000, sizeof(header));
            if (header.magic == NCZ_SECTION_MAGIC) {
                // validate section header.
                R_UNLESS(header.total_sections, Result_YatiInvalidNczSectionCount);

                buf_size = 0x4000;
                log_write("found ncz, total number of sections: %zu\n", header.total_sections);
                t->ncz_sections.resize(header.total_sections);
                R_TRY(t->Read(t->ncz_sections.data(), t->ncz_sections.size() * sizeof(ncz::Section), std::addressof(bytes_read)));

                // check for ncz block header.
                R_TRY(t->Read(std::addressof(t->ncz_block_header), sizeof(t->ncz_block_header), std::addressof(bytes_read)));
                if (t->ncz_block_header.magic != NCZ_BLOCK_MAGIC) {
                    // didn't find block, keep the data we just read in the temp buffer.
                    temp_buf.resize(sizeof(t->ncz_block_header));
                    std::memcpy(temp_buf.data(), std::addressof(t->ncz_block_header), temp_buf.size());
                    log_write("storing temp data of size: %zu\n", temp_buf.size());
                } else {
                    // validate block header.
                    R_TRY(t->ncz_block_header.IsValid());

                    // read blocks (array of block sizes).
                    std::vector<ncz::Block> blocks(t->ncz_block_header.total_blocks);
                    R_TRY(t->Read(blocks.data(), blocks.size() * sizeof(ncz::Block), std::addressof(bytes_read)));

                    // calculate offsets for each block.
                    auto block_offset = t->read_offset.load();
                    for (const auto& block : blocks) {
                        t->ncz_blocks.emplace_back(block_offset, block.size);
                        block_offset += block.size;
                    }
                }
            }
        }

        R_TRY(t->SetDecompressBuf(buf, buffer_offset, buf_size));
    }

    log_write("read success\n");
    R_SUCCEED();
}

// decompress thread handles decrypting / modifying the nca header, decompressing ncz
// and calculating the running sha256.
Result Yati::decompressFuncInternal(ThreadData* t) {
    ON_SCOPE_EXIT( t->decompress_running = false; );

    // only used for ncz files.
    auto dctx = ZSTD_createDCtx();
    ON_SCOPE_EXIT(ZSTD_freeDCtx(dctx));
    const auto chunk_size = ZSTD_DStreamOutSize();
    const ncz::Section* ncz_section{};
    const ncz::BlockInfo* ncz_block{};
    bool is_ncz{};

    s64 inflate_offset{};
    Aes128CtrContext ctx{};
    std::vector<u8> inflate_buf{};
    inflate_buf.reserve(t->max_buffer_size);

    s64 written{};
    s64 block_offset{};
    std::vector<u8> buf{};
    buf.reserve(t->max_buffer_size);

    // encrypts the nca and passes the buffer to the write thread.
    const auto ncz_flush = [&](s64 size) -> Result {
        if (!inflate_offset) {
            R_SUCCEED();
        }

        // if we are not moving the whole vector, then we need to keep
        // the remaining data.
        // rather that copying the entire vector to the write thread,
        // only copy (store) the remaining amount.
        std::vector<u8> temp_vector{};
        if (size < inflate_offset) {
            temp_vector.resize(inflate_offset - size);
            std::memcpy(temp_vector.data(), inflate_buf.data() + size, temp_vector.size());
        }

        for (s64 off = 0; off < size;) {
            if (!ncz_section || !ncz_section->InRange(written)) {
                log_write("[NCZ] looking for new section: %zu off: %zu size: %zu\n", written, off, size);
                auto it = std::ranges::find_if(t->ncz_sections, [written](auto& e){
                    log_write("\t[NCZ] checking offset: %zu size: %zu written: %zu\n", e.offset, e.size, written);
                    return e.InRange(written);
                });

                R_UNLESS(it != t->ncz_sections.cend(), Result_YatiNczSectionNotFound);
                ncz_section = &(*it);
                log_write("[NCZ] found new section: %zu\n", written);

                if (ncz_section->crypto_type >= nca::EncryptionType_AesCtr) {
                    const auto swp = std::byteswap(u64(written) >> 4);
                    u8 counter[0x16];
                    std::memcpy(counter + 0x0, ncz_section->counter, 0x8);
                    std::memcpy(counter + 0x8, &swp, 0x8);
                    aes128CtrContextCreate(&ctx, ncz_section->key, counter);
                }
            }

            const auto total_size = ncz_section->offset + ncz_section->size;
            const auto chunk_size = std::min<u64>(total_size - written, size - off);

            if (ncz_section->crypto_type >= nca::EncryptionType_AesCtr) {
                aes128CtrCrypt(&ctx, inflate_buf.data() + off, inflate_buf.data() + off, chunk_size);
            }

            written += chunk_size;
            off += chunk_size;
        }

        R_TRY(t->SetWriteBuf(inflate_buf, size, config.skip_nca_hash_verify));
        inflate_offset -= size;

        // restore remaining data to the swapped buffer.
        if (!temp_vector.empty()) {
            log_write("[NCZ] storing data size: %zu\n", temp_vector.size());
            inflate_buf = temp_vector;
        }

        R_SUCCEED();
    };

    while (t->decompress_offset < t->write_size && R_SUCCEEDED(t->GetResults())) {
        s64 decompress_buf_off{};
        R_TRY(t->GetDecompressBuf(buf, decompress_buf_off));
        if (buf.empty()) {
            break;
        }

        // do we have an nsz? if so, setup buffers.
        if (!is_ncz && !t->ncz_sections.empty()) {
            log_write("YES IT FOUND NCZ\n");
            is_ncz = true;
        }

        // if we don't have a ncz or it's before the ncz header, pass buffer directly to write
        if (!is_ncz || !decompress_buf_off) {
            // check nca header
            if (!decompress_buf_off) {
                log_write("reading nca header\n");

                log_write("verifying nca header magic\n");
                nca::Header header{};
                R_TRY(nca::DecryptHeader(buf.data(), keys, header));
                log_write("nca magic is ok! type: %u\n", header.content_type);

                // store the unmodified header.
                t->nca->header = header;

                if (!config.skip_rsa_header_fixed_key_verify) {
                    log_write("verifying nca fixed key\n");
                    R_TRY(nca::VerifyFixedKey(header));
                    log_write("nca fixed key is ok! type: %u\n", header.content_type);
                } else {
                    log_write("skipping nca verification\n");
                }

                t->write_size = header.size;
                log_write("setting placeholder size: %zu\n", t->write_size.load());
                R_TRY(ncmContentStorageSetPlaceHolderSize(std::addressof(cs), std::addressof(t->nca->placeholder_id), t->write_size));

                if (!config.ignore_distribution_bit && header.distribution_type == nca::DistributionType_GameCard) {
                    header.distribution_type = nca::DistributionType_System;
                    t->nca->modified = true;
                }

                // try and get the ticket, if the nca requires it.
                auto ticket = GetTicketCollection(header, t->tik);
                R_TRY(HasRequiredTicket(header, ticket));

                if ((config.convert_to_standard_crypto && ticket) || config.lower_master_key) {
                    t->nca->modified = true;
                    u8 keak_generation = 0;

                    if (ticket) {
                        const auto key_gen = header.GetKeyGeneration();
                        log_write("converting to standard crypto: 0x%X 0x%X\n", key_gen, header.GetKeyGeneration());

                        keys::KeyEntry title_key;
                        R_TRY(es::GetTitleKeyDecrypted(ticket->ticket, header.rights_id, key_gen, keys, title_key));

                        std::memset(header.key_area, 0, sizeof(header.key_area));
                        std::memcpy(&header.key_area[0x2], &title_key, sizeof(title_key));

                        keak_generation = key_gen;
                        ticket->required = false;
                    } else if (config.lower_master_key) {
                        R_TRY(nca::DecryptKeak(keys, header));
                    }

                    R_TRY(nca::EncryptKeak(keys, header, keak_generation));
                    std::memset(&header.rights_id, 0, sizeof(header.rights_id));
                }

                if (t->nca->modified) {
                    crypto::cryptoAes128Xts(std::addressof(header), buf.data(), keys.header_key, 0, 0x200, sizeof(header), true);
                }
            }

            written += buf.size();
            t->decompress_offset += buf.size();
            R_TRY(t->SetWriteBuf(buf, buf.size(), config.skip_nca_hash_verify));
        } else if (is_ncz) {
            u64 buf_off{};
            while (buf_off < buf.size()) {
                std::span<const u8> buffer{buf.data() + buf_off, buf.size() - buf_off};
                bool compressed = true;

                // todo: blocks need to use read offset, as the offset + size is compressed range.
                if (t->ncz_blocks.size()) {
                    if (!ncz_block || !ncz_block->InRange(decompress_buf_off)) {
                        block_offset = 0;
                        log_write("[NCZ] looking for new block: %zu\n", decompress_buf_off);
                        auto it = std::ranges::find_if(t->ncz_blocks, [decompress_buf_off](auto& e){
                            return e.InRange(decompress_buf_off);
                        });

                        R_UNLESS(it != t->ncz_blocks.cend(), Result_YatiNczBlockNotFound);
                        log_write("[NCZ] found new block: %zu off: %zd size: %zd\n", decompress_buf_off, it->offset, it->size);
                        ncz_block = &(*it);
                    }

                    // https://github.com/nicoboss/nsz/issues/79
                    auto decompressedBlockSize = 1UL << t->ncz_block_header.block_size_exponent;
                    // special handling for the last block to check it's actually compressed
                    if (ncz_block->offset == t->ncz_blocks.back().offset) {
                        log_write("[NCZ] last block special handling\n");
                        // https://github.com/nicoboss/nsz/issues/210
                        const auto remainder = t->ncz_block_header.decompressed_size % decompressedBlockSize;
                        if (remainder) {
                            decompressedBlockSize = remainder;
                        }
                    }

                    // check if this block is compressed.
                    compressed = ncz_block->size < decompressedBlockSize;

                    // clip read size as blocks can be up to 32GB in size!
                    const auto size = std::min<u64>(buffer.size(), ncz_block->size - block_offset);
                    buffer = buffer.subspan(0, size);
                }

                if (compressed) {
                    log_write("[NCZ] COMPRESSED block\n");
                    ZSTD_inBuffer input = { buffer.data(), buffer.size(), 0 };
                    while (input.pos < input.size) {
                        R_TRY(t->GetResults());

                        inflate_buf.resize(inflate_offset + chunk_size);
                        ZSTD_outBuffer output = { inflate_buf.data() + inflate_offset, chunk_size, 0 };
                        const auto res = ZSTD_decompressStream(dctx, std::addressof(output), std::addressof(input));
                        if (ZSTD_isError(res)) {
                            log_write("[NCZ] ZSTD_decompressStream() pos: %zu size: %zu res: %zd msg: %s\n", input.pos, input.size, res, ZSTD_getErrorName(res));
                        }
                        R_UNLESS(!ZSTD_isError(res), Result_YatiInvalidNczZstdError);

                        t->decompress_offset += output.pos;
                        inflate_offset += output.pos;
                        if (inflate_offset >= INFLATE_BUFFER_MAX) {
                            log_write("[NCZ] flushing compressed data: %zd vs %zd diff: %zd\n", inflate_offset, INFLATE_BUFFER_MAX, inflate_offset - INFLATE_BUFFER_MAX);
                            R_TRY(ncz_flush(INFLATE_BUFFER_MAX));
                        }
                    }
                } else {
                    inflate_buf.resize(inflate_offset + buffer.size());
                    std::memcpy(inflate_buf.data() + inflate_offset, buffer.data(), buffer.size());

                    t->decompress_offset += buffer.size();
                    inflate_offset += buffer.size();
                    if (inflate_offset >= INFLATE_BUFFER_MAX) {
                        log_write("[NCZ] flushing copy data\n");
                        R_TRY(ncz_flush(INFLATE_BUFFER_MAX));
                    }
                }

                buf_off += buffer.size();
                decompress_buf_off += buffer.size();
                block_offset += buffer.size();
            }
        }
    }

    // flush remaining data.
    if (is_ncz && inflate_offset) {
        log_write("flushing remaining\n");
        R_TRY(ncz_flush(inflate_offset));
    }

    log_write("decompress thread done!\n");

    // get final hash output.
    sha256ContextGetHash(std::addressof(t->sha256), t->nca->hash);

    R_SUCCEED();
}

// write thread writes data to the nca placeholder.
Result Yati::writeFuncInternal(ThreadData* t) {
    ON_SCOPE_EXIT( t->write_running = false; );

    std::vector<u8> buf;
    buf.reserve(t->max_buffer_size);
    const auto is_file_based_emummc = App::IsFileBaseEmummc();

    while (t->write_offset < t->write_size && R_SUCCEEDED(t->GetResults())) {
        s64 dummy_off;
        R_TRY(t->GetWriteBuf(buf, dummy_off));
        if (buf.empty()) {
            break;
        }

        s64 off{};
        while (off < buf.size() && t->write_offset < t->write_size && R_SUCCEEDED(t->GetResults())) {
            const auto wsize = std::min<s64>(t->read_buffer_size, buf.size() - off);
            R_TRY(ncmContentStorageWritePlaceHolder(std::addressof(cs), std::addressof(t->nca->placeholder_id), t->write_offset, buf.data() + off, wsize));

            off += wsize;
            t->write_offset += wsize;
            ueventSignal(t->GetProgressEvent());

            // todo: check how much time elapsed and sleep the diff
            // rather than always sleeping a fixed amount.
            // ie, writing a small buffer (nca header) should not sleep the full 2 ms.
            if (is_file_based_emummc) {
                svcSleepThread(2e+6); // 2ms
            }
        }
    }

    log_write("finished write thread!\n");
    R_SUCCEED();
}

void readFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetReadResult(t->yati->readFuncInternal(t));
    log_write("read thread returned now\n");
}

void decompressFunc(void* d) {
    log_write("hello decomp thread func\n");
    auto t = static_cast<ThreadData*>(d);
    t->SetDecompressResult(t->yati->decompressFuncInternal(t));
    log_write("decompress thread returned now\n");
}

void writeFunc(void* d) {
    auto t = static_cast<ThreadData*>(d);
    t->SetWriteResult(t->yati->writeFuncInternal(t));
    log_write("write thread returned now\n");
}

// stdio-like wrapper for std::vector
struct BufHelper {
    BufHelper() = default;
    BufHelper(std::span<const u8> data) {
        write(data);
    }

    void write(const void* data, u64 size) {
        if (offset + size >= buf.size()) {
            buf.resize(offset + size);
        }
        std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }

    void write(std::span<const u8> data) {
        write(data.data(), data.size());
    }

    void seek(u64 where_to) {
        offset = where_to;
    }

    [[nodiscard]]
    auto tell() const {
        return offset;
    }

    std::vector<u8> buf{};
    u64 offset{};
};

Yati::Yati(ui::ProgressBox* _pbox, source::Base* _source) : pbox{_pbox}, source{_source} {
    App::SetAutoSleepDisabled(true);
}

Yati::~Yati() {
    // released in reverse order of acquisition, and only what Setup() got as
    // far as acquiring, see the comment on the flags.
    // note: the ncm handles below don't need the same treatment, they are
    // zero initialised and closing an unopened one is a no-op.
    if (es_init) {
        es::Exit();
    }

    if (ns_init) {
        ns::Exit();
    }

    if (spl_crypto_init) {
        splCryptoExit();
    }

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        ncmContentMetaDatabaseClose(std::addressof(ncm_db[i]));
        ncmContentStorageClose(std::addressof(ncm_cs[i]));
    }

    App::SetAutoSleepDisabled(false);

    // force update the game menu, as we may have installed a game.
    ui::menu::game::SignalChange();
    ui::menu::ownfoil::SignalInstalled();
}

Result Yati::Setup(const ConfigOverride& override) {
    config.sd_card_install = override.sd_card_install.value_or(App::GetApp()->m_install_sd.Get());
    config.allow_downgrade = App::GetApp()->m_allow_downgrade.Get();
    config.skip_if_already_installed = App::GetApp()->m_skip_if_already_installed.Get();
    config.ticket_only = App::GetApp()->m_ticket_only.Get();
    config.skip_base = App::GetApp()->m_skip_base.Get();
    config.skip_patch = App::GetApp()->m_skip_patch.Get();
    config.skip_addon = App::GetApp()->m_skip_addon.Get();
    config.skip_data_patch = App::GetApp()->m_skip_data_patch.Get();
    config.skip_ticket = App::GetApp()->m_skip_ticket.Get();
    config.skip_nca_hash_verify = override.skip_nca_hash_verify.value_or(App::GetApp()->m_skip_nca_hash_verify.Get());
    config.skip_rsa_header_fixed_key_verify = override.skip_rsa_header_fixed_key_verify.value_or(App::GetApp()->m_skip_rsa_header_fixed_key_verify.Get());
    config.skip_rsa_npdm_fixed_key_verify = override.skip_rsa_npdm_fixed_key_verify.value_or(App::GetApp()->m_skip_rsa_npdm_fixed_key_verify.Get());
    config.ignore_distribution_bit = override.ignore_distribution_bit.value_or(App::GetApp()->m_ignore_distribution_bit.Get());
    config.convert_to_common_ticket = override.convert_to_common_ticket.value_or(App::GetApp()->m_convert_to_common_ticket.Get());
    config.convert_to_standard_crypto = override.convert_to_standard_crypto.value_or(App::GetApp()->m_convert_to_standard_crypto.Get());
    config.lower_master_key = override.lower_master_key.value_or(App::GetApp()->m_lower_master_key.Get());
    config.lower_system_version = override.lower_system_version.value_or(App::GetApp()->m_lower_system_version.Get());
    config.title_ids = override.title_ids;
    storage_id = config.sd_card_install ? NcmStorageId_SdCard : NcmStorageId_BuiltInUser;

    R_TRY(source->GetOpenResult());

    R_TRY(splCryptoInitialize());
    spl_crypto_init = true;

    R_TRY(ns::Initialize());
    ns_init = true;

    R_TRY(es::Initialize());
    es_init = true;

    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        R_TRY(ncmOpenContentMetaDatabase(std::addressof(ncm_db[i]), NCM_STORAGE_IDS[i]));
        R_TRY(ncmOpenContentStorage(std::addressof(ncm_cs[i]), NCM_STORAGE_IDS[i]));
    }

    cs = ncm_cs[config.sd_card_install];
    db = ncm_db[config.sd_card_install];

    R_TRY(parse_keys(keys, true));
    R_SUCCEED();
}

Result Yati::InstallNcaInternal(std::span<TikCollection> tickets, NcaCollection& nca) {
    if (config.skip_if_already_installed || config.ticket_only) {
        R_TRY(ncmContentStorageHas(std::addressof(cs), std::addressof(nca.skipped), std::addressof(nca.content_id)));
        if (nca.skipped) {
            log_write("\tskipped nca as it's already installed ncmContentStorageHas()\n");
            R_TRY(ncmContentStorageReadContentIdFile(std::addressof(cs), std::addressof(nca.header), sizeof(nca.header), std::addressof(nca.content_id), 0));
            crypto::cryptoAes128Xts(std::addressof(nca.header), std::addressof(nca.header), keys.header_key, 0, 0x200, sizeof(nca.header), false);

            R_TRY(HasRequiredTicket(nca.header, tickets));
            R_SUCCEED();
        }
    }

    log_write("generateing placeholder\n");
    R_TRY(ncmContentStorageGeneratePlaceHolderId(std::addressof(cs), std::addressof(nca.placeholder_id)));
    log_write("creating placeholder\n");
    R_TRY(ncmContentStorageCreatePlaceHolder(std::addressof(cs), std::addressof(nca.content_id), std::addressof(nca.placeholder_id), nca.size));

    log_write("opening thread\n");
    ThreadData t_data{this, tickets, std::addressof(nca)};

    #define READ_THREAD_CORE 1
    #define DECOMPRESS_THREAD_CORE 2
    #define WRITE_THREAD_CORE 0
    // #define READ_THREAD_CORE 2
    // #define DECOMPRESS_THREAD_CORE 2
    // #define WRITE_THREAD_CORE 2

    Thread t_read{};
    R_TRY(utils::CreateThread(&t_read, readFunc, std::addressof(t_data), 1024*64));
    ON_SCOPE_EXIT(threadClose(&t_read));

    Thread t_decompress{};
    R_TRY(utils::CreateThread(&t_decompress, decompressFunc, std::addressof(t_data), 1024*64));
    ON_SCOPE_EXIT(threadClose(&t_decompress));

    Thread t_write{};
    R_TRY(utils::CreateThread(&t_write, writeFunc, std::addressof(t_data), 1024*64));
    ON_SCOPE_EXIT(threadClose(&t_write));

    log_write("starting threads\n");
    R_TRY(threadStart(std::addressof(t_read)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_read)));

    R_TRY(threadStart(std::addressof(t_decompress)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_decompress)));

    R_TRY(threadStart(std::addressof(t_write)));
    ON_SCOPE_EXIT(threadWaitForExit(std::addressof(t_write)));

    const auto waiter_progress = waiterForUEvent(t_data.GetProgressEvent());
    const auto waiter_cancel = waiterForUEvent(pbox->GetCancelEvent());
    const auto waiter_done = waiterForUEvent(t_data.GetDoneEvent());

    for (;;) {
        s32 idx;
        if (R_FAILED(waitMulti(&idx, UINT64_MAX, waiter_progress, waiter_cancel, waiter_done))) {
            break;
        }

        if (!idx) {
            pbox->UpdateTransfer(t_data.GetWriteOffset(), t_data.GetWriteSize());
        } else {
            break;
        }
    }

    // wait for all threads to close.
    log_write("waiting for threads to close\n");
    while (t_data.IsAnyRunning()) {
        t_data.WakeAllThreads();
        pbox->Yield();

        if (R_FAILED(waitSingleHandle(t_read.handle, 1000))) {
            continue;
        } else if (R_FAILED(waitSingleHandle(t_decompress.handle, 1000))) {
            continue;
        } else if (R_FAILED(waitSingleHandle(t_write.handle, 1000))) {
            continue;
        }
        break;
    }
    log_write("threads closed\n");

    // if any of the threads failed, wake up all threads so they can exit.
    if (R_FAILED(t_data.GetResults())) {
        log_write("some reads failed, waking threads: %s\n", nca.name.c_str());
        log_write("returning due to fail: %s\n", nca.name.c_str());
        return t_data.GetResults();
    }
    R_TRY(t_data.GetResults());

    NcmContentId content_id{};
    std::memcpy(std::addressof(content_id), nca.hash, sizeof(content_id));

    log_write("old id: %s new id: %s\n", utils::hexIdToStr(nca.content_id).str, utils::hexIdToStr(content_id).str);
    if (!config.skip_nca_hash_verify && !nca.modified) {
        if (std::memcmp(&nca.content_id, nca.hash, sizeof(nca.content_id))) {
            log_write("nca hash is invalid!!!!\n");
            R_UNLESS(!std::memcmp(&nca.content_id, nca.hash, sizeof(nca.content_id)), Result_YatiInvalidNcaSha256);
        } else {
            log_write("nca hash is valid!\n");
        }
    } else {
        log_write("skipping nca sha256 verify\n");
    }

    R_SUCCEED();
}

Result Yati::InstallNca(std::span<TikCollection> tickets, NcaCollection& nca) {
    log_write("in install nca\n");
    pbox->NewTransfer(nca.name);
    keys::parse_hex_key(std::addressof(nca.content_id), nca.name.c_str());

    R_TRY(InstallNcaInternal(tickets, nca));

    fs::FsPath path;
    if (nca.skipped) {
        R_TRY(ncmContentStorageGetPath(std::addressof(cs), path, sizeof(path), std::addressof(nca.content_id)));
    } else {
        R_TRY(ncmContentStorageFlushPlaceHolder(std::addressof(cs)));
        R_TRY(ncmContentStorageGetPlaceHolderPath(std::addressof(cs), path, sizeof(path), std::addressof(nca.placeholder_id)));
    }

    if (nca.header.content_type == nca::ContentType_Program) {
        // todo: verify npdm key.
    } else if (nca.header.content_type == nca::ContentType_Control) {
        NacpLanguageEntry entry;
        std::vector<u8> icon;
        // this may fail if tickets aren't installed and the nca uses title key crypto.
        if (R_SUCCEEDED(nca::ParseControl(path, nca.header.program_id, &entry, sizeof(entry), &icon))) {
            pbox->SetTitle(entry.name).SetImageData(icon);
        }
    }

    R_SUCCEED();
}

Result Yati::InstallCnmtNca(std::span<TikCollection> tickets, CnmtCollection& cnmt, const container::Collections& collections) {
    R_TRY(InstallNca(tickets, cnmt));

    fs::FsPath path;
    if (cnmt.skipped) {
        R_TRY(ncmContentStorageGetPath(std::addressof(cs), path, sizeof(path), std::addressof(cnmt.content_id)));
    } else {
        R_TRY(ncmContentStorageFlushPlaceHolder(std::addressof(cs)));
        R_TRY(ncmContentStorageGetPlaceHolderPath(std::addressof(cs), path, sizeof(path), std::addressof(cnmt.placeholder_id)));
    }

    ncm::PackagedContentMeta header;
    std::vector<NcmPackagedContentInfo> infos;
    R_TRY(nca::ParseCnmt(path, cnmt.header.program_id, header, cnmt.extended_header, infos));

    for (const auto& packed_info : infos) {
        const auto& info = packed_info.info;
        if (info.content_type == NcmContentType_DeltaFragment) {
            continue;
        }

        const auto str = utils::hexIdToStr(info.content_id);
        const auto it = std::ranges::find_if(collections, [&str](auto& e){
            return e.name.find(str.str) != e.name.npos;
        });

        R_UNLESS(it != collections.cend(), Result_YatiNcaNotFound);

        log_write("found: %s\n", str.str);
        cnmt.infos.emplace_back(packed_info);
        auto& nca = cnmt.ncas.emplace_back(*it);
        nca.type = info.content_type;
    }

    // update header
    cnmt.meta_header = header.meta_header;
    cnmt.meta_header.content_count = cnmt.infos.size() + 1;
    cnmt.meta_header.storage_id = 0;

    cnmt.key.id = header.title_id;
    cnmt.key.version = header.title_version;
    cnmt.key.type = header.meta_type;
    cnmt.key.install_type = NcmContentInstallType_Full;
    std::memset(cnmt.key.padding, 0, sizeof(cnmt.key.padding));

    cnmt.content_info.content_id = cnmt.content_id;
    cnmt.content_info.content_type = NcmContentType_Meta;
    cnmt.content_info.attr = 0;
    ncmU64ToContentInfoSize(cnmt.size, &cnmt.content_info);
    cnmt.content_info.id_offset = 0;

    if (config.lower_system_version) {
        auto extended_header = (ncm::ExtendedHeader*)cnmt.extended_header.data();
        log_write("patching version\n");
        if (cnmt.key.type == NcmContentMetaType_Application) {
            extended_header->application.required_system_version = 0;
        } else if (cnmt.key.type == NcmContentMetaType_Patch) {
            extended_header->patch.required_system_version = 0;
        }
    }

    // sort ncas
    const auto sorter = [](NcaCollection& lhs, NcaCollection& rhs) -> bool {
        return lhs.type > rhs.type;
    };

    std::ranges::sort(cnmt.ncas, sorter);

    log_write("found all cnmts\n");
    R_SUCCEED();
}

Result Yati::ParseTicketsIntoCollection(std::vector<TikCollection>& tickets, const container::Collections& collections, bool read_data) {
    for (const auto& collection : collections) {
        if (collection.name.ends_with(".tik")) {
            TikCollection entry{};
            keys::parse_hex_key(entry.rights_id.c, collection.name.c_str());
            const auto str = collection.name.substr(0, collection.name.length() - 4) + ".cert";

            const auto cert = std::ranges::find_if(collections, [&str](auto& e){
                return e.name.find(str) != e.name.npos;
            });

            R_UNLESS(cert != collections.cend(), Result_YatiCertNotFound);
            entry.ticket.resize(collection.size);
            entry.cert.resize(cert->size);

            // only supported on non-stream installs.
            if (read_data) {
                u64 bytes_read;
                R_TRY(source->Read(entry.ticket.data(), collection.offset, entry.ticket.size(), &bytes_read));
                R_TRY(source->Read(entry.cert.data(), cert->offset, entry.cert.size(), &bytes_read));
            }

            tickets.emplace_back(entry);
        }
    }

    R_SUCCEED();
}

Result Yati::GetLatestVersion(const CnmtCollection& cnmt, u32& version_out, bool& skip) {
    const auto app_id = ncm::GetAppId(cnmt.key);
    version_out = cnmt.key.version;

    for (auto& db : ncm_db) {
        s32 db_list_total;
        s32 db_list_count;
        std::vector<NcmContentMetaKey> keys(1);
        if (R_SUCCEEDED(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), NcmContentMetaType_Unknown, app_id, 0, UINT64_MAX, NcmContentInstallType_Full))) {
            if (db_list_total != keys.size()) {
                keys.resize(db_list_total);
                if (keys.size()) {
                    R_TRY(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), NcmContentMetaType_Unknown, app_id, 0, UINT64_MAX, NcmContentInstallType_Full));
                }
            }

            for (auto& key : keys) {
                log_write("found record: %016lX type: %u version: %u\n", key.id, key.type, key.version);

                if (key.id == cnmt.key.id && cnmt.key.version == key.version && config.skip_if_already_installed) {
                    log_write("skipping as already installed\n");
                    skip = true;
                }

                // check if we are downgrading
                if (cnmt.key.type == NcmContentMetaType_Patch) {
                    if (cnmt.key.type == key.type && cnmt.key.version < key.version && !config.allow_downgrade) {
                        log_write("skipping due to it being lower\n");
                        skip = true;
                    }
                } else {
                    version_out = std::max(version_out, key.version);
                }
            }
        }
    }

    R_SUCCEED();
}

Result Yati::ShouldSkip(const CnmtCollection& cnmt, bool& skip) {
    if (!skip && config.skip_if_already_installed) {
        bool has;
        R_TRY(ncmContentMetaDatabaseHas(std::addressof(db), std::addressof(has), std::addressof(cnmt.key)));
        if (has) {
            log_write("\tskipping: [ncmContentMetaDatabaseHas()]\n");
            skip = true;
        }
    }

    // skip invalid types
    if (!skip) {
        if (!(cnmt.key.type & 0x80)) {
            log_write("\tskipping: invalid: %u\n", cnmt.key.type);
            skip = true;
        } else if (!config.title_ids.empty() && std::ranges::find(config.title_ids, cnmt.key.id) == config.title_ids.end()) {
            log_write("\tskipping: not selected: %016lX\n", cnmt.key.id);
            skip = true;
        } else if (config.skip_base && cnmt.key.type == NcmContentMetaType_Application) {
            log_write("\tskipping: [NcmContentMetaType_Application]\n");
            skip = true;
        } else if (config.skip_patch && cnmt.key.type == NcmContentMetaType_Patch) {
            log_write("\tskipping: [NcmContentMetaType_Application]\n");
            skip = true;
        } else if (config.skip_addon && cnmt.key.type == NcmContentMetaType_AddOnContent) {
            log_write("\tskipping: [NcmContentMetaType_AddOnContent]\n");
            skip = true;
        } else if (config.skip_data_patch && cnmt.key.type == NcmContentMetaType_DataPatch) {
            log_write("\tskipping: [NcmContentMetaType_DataPatch]\n");
            skip = true;
        }
    }

    R_SUCCEED();
}

Result Yati::ImportTickets(std::span<TikCollection> tickets) {
    for (auto& ticket : tickets) {
        if (ticket.required || config.ticket_only) {
            if (config.skip_ticket) {
                log_write("WARNING: skipping ticket install, but it's required!\n");
            } else {
                if (!ticket.patched) {
                    log_write("patching ticket\n");
                    R_TRY(es::PatchTicket(ticket.ticket, ticket.cert, ticket.key_gen, keys, config.convert_to_common_ticket));
                    ticket.patched = true;
                }

                log_write("installing ticket\n");
                R_TRY(es::ImportTicket(ticket.ticket.data(), ticket.ticket.size(), ticket.cert.data(), ticket.cert.size()));
                ticket.required = false;
            }
        }
    }

    R_SUCCEED();
}

Result Yati::RemoveInstalledNcas(const CnmtCollection& cnmt) {
    const auto app_id = ncm::GetAppId(cnmt.key);

    // remove current entries (if any).
    s32 db_list_total;
    s32 db_list_count;
    u64 id_min = cnmt.key.id;
    u64 id_max = cnmt.key.id;

    // if installing a patch, remove all previously installed patches.
    if (cnmt.key.type == NcmContentMetaType_Patch) {
        id_min = 0;
        id_max = UINT64_MAX;
    }

    log_write("listing keys\n");
    for (size_t i = 0; i < std::size(NCM_STORAGE_IDS); i++) {
        auto& cs = ncm_cs[i];
        auto& db = ncm_db[i];

        std::vector<NcmContentMetaKey> keys(1);
        R_TRY(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), static_cast<NcmContentMetaType>(cnmt.key.type), app_id, id_min, id_max, NcmContentInstallType_Full));

        if (db_list_total != keys.size()) {
            keys.resize(db_list_total);
            if (keys.size()) {
                R_TRY(ncmContentMetaDatabaseList(std::addressof(db), std::addressof(db_list_total), std::addressof(db_list_count), keys.data(), keys.size(), static_cast<NcmContentMetaType>(cnmt.key.type), app_id, id_min, id_max, NcmContentInstallType_Full));
            }
        }

        for (const auto& key : keys) {
            log_write("found key: 0x%016lX type: %u version: %u\n", key.id, key.type, key.version);
            NcmContentMetaHeader header;
            u64 out_size;
            log_write("trying to get from db\n");
            R_TRY(ncmContentMetaDatabaseGet(std::addressof(db), std::addressof(key), std::addressof(out_size), std::addressof(header), sizeof(header)));
            R_UNLESS(out_size == sizeof(header), Result_YatiNcmDbCorruptHeader);
            log_write("trying to list infos\n");

            std::vector<NcmContentInfo> infos(header.content_count);
            s32 content_info_out;
            R_TRY(ncmContentMetaDatabaseListContentInfo(std::addressof(db), std::addressof(content_info_out), infos.data(), infos.size(), std::addressof(key), 0));
            R_UNLESS(content_info_out == infos.size(), Result_YatiNcmDbCorruptInfos);
            log_write("size matches\n");

            for (const auto& info : infos) {
                const auto it = std::ranges::find_if(cnmt.ncas, [&info](auto& e){
                    return !std::memcmp(&e.content_id, &info.content_id, sizeof(e.content_id));
                });

                // don't delete the nca if we skipped the install.
                if ((it != cnmt.ncas.cend() && it->skipped) || (!std::memcmp(&cnmt.content_id, &info.content_id, sizeof(cnmt.content_id)) && cnmt.skipped)) {
                    continue;
                }

                R_TRY(ncm::Delete(std::addressof(cs), std::addressof(info.content_id)));
            }

            log_write("trying to remove it\n");
            R_TRY(ncmContentMetaDatabaseRemove(std::addressof(db), std::addressof(key)));
            R_TRY(ncmContentMetaDatabaseCommit(std::addressof(db)));
            log_write("all done with this key\n\n");
        }
    }

    log_write("done with keys\n");
    R_SUCCEED();
}

Result Yati::RegisterNcasAndPushRecord(const CnmtCollection& cnmt, u32 latest_version_num) {
    const auto app_id = ncm::GetAppId(cnmt.key);

    // register all nca's
    if (!cnmt.skipped) {
        log_write("registering cnmt nca\n");
        R_TRY(ncm::Register(std::addressof(cs), std::addressof(cnmt.content_id), std::addressof(cnmt.placeholder_id)));
        log_write("registered cnmt nca\n");
    }

    for (auto& nca : cnmt.ncas) {
        if (!nca.skipped && nca.type != NcmContentType_DeltaFragment) {
            log_write("registering nca: %s\n", nca.name.c_str());
            R_TRY(ncm::Register(std::addressof(cs), std::addressof(nca.content_id), std::addressof(nca.placeholder_id)));
            log_write("registered nca: %s\n", nca.name.c_str());
        }
    }

    log_write("register'd all ncas\n");

    // build ncm meta and push to the database.
    BufHelper buf{};
    buf.write(std::addressof(cnmt.meta_header), sizeof(cnmt.meta_header));
    buf.write(cnmt.extended_header.data(), cnmt.extended_header.size());
    buf.write(std::addressof(cnmt.content_info), sizeof(cnmt.content_info));

    for (auto& info : cnmt.infos) {
        buf.write(std::addressof(info.info), sizeof(info.info));
    }

    pbox->NewTransfer("Updating ncm database"_i18n);
    R_TRY(ncmContentMetaDatabaseSet(std::addressof(db), std::addressof(cnmt.key), buf.buf.data(), buf.tell()));
    R_TRY(ncmContentMetaDatabaseCommit(std::addressof(db)));

    // push record.
    ncm::ContentStorageRecord content_storage_record{};
    content_storage_record.key = cnmt.key;
    content_storage_record.storage_id = storage_id;
    pbox->NewTransfer("Pushing application record"_i18n);

    R_TRY(ns::PushApplicationRecord(app_id, std::addressof(content_storage_record), 1));
    if (hosversionAtLeast(6,0,0)) {
        R_TRY(avmInitialize());
        ON_SCOPE_EXIT(avmExit());

        R_TRY(avmPushLaunchVersion(app_id, latest_version_num));
    }
    log_write("pushed\n");

    R_SUCCEED();
}

Result InstallInternal(ui::ProgressBox* pbox, source::Base* source, const container::Collections& collections, const ConfigOverride& override) {
    auto yati = std::make_unique<Yati>(pbox, source);
    R_TRY(yati->Setup(override));

    std::vector<TikCollection> tickets{};
    R_TRY(yati->ParseTicketsIntoCollection(tickets, collections, true));

    std::vector<CnmtCollection> cnmts{};
    for (const auto& collection : collections) {
        log_write("found collection: %s\n", collection.name.c_str());
        if (collection.name.ends_with(".cnmt.nca") || collection.name.ends_with(".cnmt.ncz")) {
            auto& cnmt = cnmts.emplace_back(NcaCollection{collection});
            cnmt.type = NcmContentType_Meta;
        }
    }

    for (auto& cnmt : cnmts) {
        ON_SCOPE_EXIT(
            ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(cnmt.placeholder_id));
            for (auto& nca : cnmt.ncas) {
                ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(nca.placeholder_id));
            }
        );

        R_TRY(yati->InstallCnmtNca(tickets, cnmt, collections));

        u32 latest_version_num;
        bool skip = false;
        R_TRY(yati->GetLatestVersion(cnmt, latest_version_num, skip));
        R_TRY(yati->ShouldSkip(cnmt, skip));

        if (skip) {
            log_write("skipping install!\n");
            continue;
        }

        log_write("installing nca's\n");
        for (auto& nca : cnmt.ncas) {
            R_TRY(yati->InstallNca(tickets, nca));
        }

        R_TRY(yati->ImportTickets(tickets));
        R_TRY(yati->RemoveInstalledNcas(cnmt));
        R_TRY(yati->RegisterNcasAndPushRecord(cnmt, latest_version_num));
    }

    log_write("success!\n");
    R_SUCCEED();
}

Result InstallInternalStream(ui::ProgressBox* pbox, source::Base* source, container::Collections collections, const ConfigOverride& override) {
    auto yati = std::make_unique<Yati>(pbox, source);
    R_TRY(yati->Setup(override));

    // not supported with stream installs (yet).
    yati->config.skip_if_already_installed = false;
    yati->config.convert_to_standard_crypto = false;
    yati->config.lower_master_key = false;

    std::vector<NcaCollection> ncas{};
    std::vector<CnmtCollection> cnmts{};
    std::vector<TikCollection> tickets{};

    ON_SCOPE_EXIT(
        for (const auto& cnmt : cnmts) {
            ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(cnmt.placeholder_id));
        }

        for (const auto& nca : ncas) {
            ncmContentStorageDeletePlaceHolder(std::addressof(yati->cs), std::addressof(nca.placeholder_id));
        }
    );

    // fill ticket entries, the data will be filled later on.
    R_TRY(yati->ParseTicketsIntoCollection(tickets, collections, false));

    // sort based on lowest offset.
    const auto sorter = [](const container::CollectionEntry& lhs, const container::CollectionEntry& rhs) -> bool {
        return lhs.offset < rhs.offset;
    };

    std::ranges::sort(collections, sorter);

    for (const auto& collection : collections) {
        if (collection.name.ends_with(".nca") || collection.name.ends_with(".ncz")) {
            auto& nca = ncas.emplace_back(NcaCollection{collection});
            if (collection.name.ends_with(".cnmt.nca") || collection.name.ends_with(".cnmt.ncz")) {
                auto& cnmt = cnmts.emplace_back(nca);
                cnmt.type = NcmContentType_Meta;
                R_TRY(yati->InstallCnmtNca(tickets, cnmt, collections));
            } else {
                R_TRY(yati->InstallNca(tickets, nca));
            }
        } else if (collection.name.ends_with(".tik") || collection.name.ends_with(".cert")) {
            FsRightsId rights_id{};
            keys::parse_hex_key(rights_id.c, collection.name.c_str());
            const auto str = collection.name.substr(0, collection.name.length() - 4) + ".cert";

            auto entry = std::ranges::find_if(tickets, [&rights_id](auto& e){
                return !std::memcmp(&rights_id, &e.rights_id, sizeof(rights_id));
            });

            // this will never fail...but just in case.
            R_UNLESS(entry != tickets.end(), Result_YatiCertNotFound);

            u64 bytes_read;
            if (collection.name.ends_with(".tik")) {
                R_TRY(source->Read(entry->ticket.data(), collection.offset, entry->ticket.size(), &bytes_read));
            } else {
                R_TRY(source->Read(entry->cert.data(), collection.offset, entry->cert.size(), &bytes_read));
            }
        }
    }

    for (auto& cnmt : cnmts) {
        // copy nca structs into cnmt.
        for (auto& cnmt_nca : cnmt.ncas) {
            auto it = std::ranges::find_if(ncas, [&cnmt_nca](auto& e){
                return e.name == cnmt_nca.name;
            });

            R_UNLESS(it != ncas.cend(), Result_YatiNczSectionNotFound);
            const auto type = cnmt_nca.type;
            cnmt_nca = *it;
            cnmt_nca.type = type;
        }

        u32 latest_version_num;
        bool skip = false;
        R_TRY(yati->GetLatestVersion(cnmt, latest_version_num, skip));
        R_TRY(yati->ShouldSkip(cnmt, skip));

        if (skip) {
            log_write("skipping install!\n");
            continue;
        }

        R_TRY(yati->ImportTickets(tickets));
        R_TRY(yati->RemoveInstalledNcas(cnmt));
        R_TRY(yati->RegisterNcasAndPushRecord(cnmt, latest_version_num));
    }

    log_write("success!\n");
    R_SUCCEED();
}

} // namespace

Result InstallFromFile(ui::ProgressBox* pbox, fs::Fs* fs, const fs::FsPath& path, const ConfigOverride& override) {
    auto source = std::make_unique<source::File>(fs, path);
    R_TRY(source->GetOpenResult());
    s64 source_size{-1};
    if (R_FAILED(source->GetSize(&source_size))) {
        source_size = -1;
    }
    // auto source = std::make_unique<source::StreamFile>(fs, path, override); // enable for testing.
    return InstallFromSource(pbox, source.get(), path, override, source_size);
}

Result InstallFromSource(ui::ProgressBox* pbox, source::Base* source, const fs::FsPath& path, const ConfigOverride& override, s64 source_size) {
    const auto ext = std::strrchr(path.s, '.');
    R_UNLESS(ext, Result_YatiContainerNotFound);

    if (!strcasecmp(ext, ".msp")) {
        return msp::Install(pbox, source, source_size);
    }

    std::unique_ptr<container::Base> container;
    if (!strcasecmp(ext, ".nsp") || !strcasecmp(ext, ".nsz")) {
        container = std::make_unique<container::Nsp>(source);
    } else if (!strcasecmp(ext, ".xci") || !strcasecmp(ext, ".xcz")) {
        container = std::make_unique<container::Xci>(source);
    }

    R_UNLESS(container, Result_YatiContainerNotFound);
    return InstallFromContainer(pbox, container.get(), override);
}

Result InstallFromContainer(ui::ProgressBox* pbox, container::Base* container, const ConfigOverride& override) {
    container::Collections collections;
    R_TRY(container->GetCollections(collections));
    return InstallFromCollections(pbox, container->GetSource(), collections, override);
}

Result InstallFromCollections(ui::ProgressBox* pbox, source::Base* source, const container::Collections& collections, const ConfigOverride& override) {
    if (source->IsStream()) {
        return InstallInternalStream(pbox, source, collections, override);
    } else {
        return InstallInternal(pbox, source, collections, override);
    }
}

} // namespace sphaira::yati
