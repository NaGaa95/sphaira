#include "yati/nx/ncz.hpp"

#include "defines.hpp"
#include "log.hpp"

#include <cstring>

namespace sphaira::ncz {

NczBlockReader::NczBlockReader(const Header& header, const Sections& sections, const BlockHeader& block_header, const Blocks& blocks, u64 offset, const std::shared_ptr<yati::source::Base>& source)
: m_header{header}
, m_sections{sections}
, m_block_header{block_header}
, m_blocks{blocks}
, m_block_offset{offset}
, m_source{source} {
    // calculate the block size.
    m_block_size = 1UL << m_block_header.block_size_exponent;

    // setup lru block cache.
    // this isn't needed in sphaira as i am usually reading in 4mb chunks.
    const auto max_lru_total_size = 1024*1024*32;
    const auto lru_count = std::max<s64>(1, max_lru_total_size / m_block_size);
    m_lru_data.resize(lru_count);
    m_lru.Init(m_lru_data);

    // calculate offsets for each block.
    auto block_offset = offset;
    for (const auto& block : m_blocks) {
        m_block_infos.emplace_back(block_offset, block.size);
        block_offset += block.size;
    }
}

Result NczBlockReader::Read(void *_buf, s64 off, s64 size, u64* bytes_read_out) {
    *bytes_read_out = 0;
    u8* buf = (u8*)_buf;

    // todo: handle case where the read is < 0x4000.
    R_UNLESS(off >= NCZ_NORMAL_SIZE, 6);
    off -= NCZ_NORMAL_SIZE;

    while (size) {
        // see if we have a cached block.
        LruData* lru_data{};
        for (auto list = m_lru.begin(); list; list = list->next) {
            if (list->data->InRange(off)) {
                lru_data = list->data;
                m_lru.Update(list);
                break;
            }
        }

        // otherwise, read new block.
        if (!lru_data) {
            // get block id and ensure we are in bounds.
            const auto block_id = off / m_block_size;
            R_UNLESS(block_id < m_block_infos.size(), Result_YatiInvalidNczBlockTotal);
            const auto& block = m_block_infos[block_id];

            // read entire block.
            std::vector<u8> temp(block.size);
            R_TRY(m_source->Read2(temp.data(), block.offset, temp.size()));

            // https://github.com/nicoboss/nsz/issues/79
            auto decompressedBlockSize = m_block_size;
            // special handling for the last block to check it's actually compressed
            if (block_id == m_block_infos.size() - 1) {
                log_write("[NCZ] last block special handling\n");
                // https://github.com/nicoboss/nsz/issues/210
                const auto remainder = m_block_header.decompressed_size % decompressedBlockSize;
                if (remainder) {
                    decompressedBlockSize = remainder;
                }
            }

            // new block.
            lru_data = m_lru.GetNextFree();
            lru_data->offset = static_cast<s64>(block_id) * m_block_size;

            // check if this block is compressed.
            const auto compressed = block.size < decompressedBlockSize;

            if (compressed) {
                // decompress block.
                lru_data->data.resize(decompressedBlockSize);
                const auto res = ZSTD_decompress(lru_data->data.data(), lru_data->data.size(), temp.data(), temp.size());

                // the output should be exactly the size of the block.
                R_UNLESS(!ZSTD_isError(res), Result_YatiInvalidNczZstdError);
                R_UNLESS(res == decompressedBlockSize, 3);
            } else {
                // saves a copy by swapping the vector.
                std::swap(lru_data->data, temp);
            }
        }

        const auto buf_off = off % m_block_size;
        const auto rsize = std::min<s64>(size, lru_data->data.size() - buf_off);
        std::memcpy(buf, lru_data->data.data() + buf_off, rsize);

        size -= rsize;
        off += rsize;
        buf += rsize;
        *bytes_read_out += rsize;
    }

    R_SUCCEED();
}

} // namespace sphaira::ncz
