#include "yati/nx/nca.hpp"
#include "yati/nx/crypto.hpp"
#include "yati/nx/es.hpp"
#include "yati/nx/nxdumptool_rsa.h"
#include "utils/utils.hpp"
#include "log.hpp"

#include <nxtc.h>
#include <cstdlib>

namespace sphaira::nca {
namespace {

constexpr u8 g_key_area_key_application_source[0x10] = { 0x7F, 0x59, 0x97, 0x1E, 0x62, 0x9F, 0x36, 0xA1, 0x30, 0x98, 0x06, 0x6F, 0x21, 0x44, 0xC3, 0x0D };
constexpr u8 g_key_area_key_ocean_source[0x10] = { 0x32, 0x7D, 0x36, 0x08, 0x5A, 0xD1, 0x75, 0x8D, 0xAB, 0x4E, 0x6F, 0xBA, 0xA5, 0x55, 0xD8, 0x82 };
constexpr u8 g_key_area_key_system_source[0x10] = { 0x87, 0x45, 0xF1, 0xBB, 0xA6, 0xBE, 0x79, 0x64, 0x7D, 0x04, 0x8B, 0xA6, 0x7B, 0x5F, 0xDA, 0x4A };

constexpr const u8* g_key_area_key[] = {
    g_key_area_key_application_source,
    g_key_area_key_ocean_source,
    g_key_area_key_system_source
};

const unsigned char nca_hdr_fixed_key_moduli_retail[0x2][0x100] = { /* Fixed RSA key used to validate NCA signature 0. */
    {
        0xBF, 0xBE, 0x40, 0x6C, 0xF4, 0xA7, 0x80, 0xE9, 0xF0, 0x7D, 0x0C, 0x99, 0x61, 0x1D, 0x77, 0x2F,
        0x96, 0xBC, 0x4B, 0x9E, 0x58, 0x38, 0x1B, 0x03, 0xAB, 0xB1, 0x75, 0x49, 0x9F, 0x2B, 0x4D, 0x58,
        0x34, 0xB0, 0x05, 0xA3, 0x75, 0x22, 0xBE, 0x1A, 0x3F, 0x03, 0x73, 0xAC, 0x70, 0x68, 0xD1, 0x16,
        0xB9, 0x04, 0x46, 0x5E, 0xB7, 0x07, 0x91, 0x2F, 0x07, 0x8B, 0x26, 0xDE, 0xF6, 0x00, 0x07, 0xB2,
        0xB4, 0x51, 0xF8, 0x0D, 0x0A, 0x5E, 0x58, 0xAD, 0xEB, 0xBC, 0x9A, 0xD6, 0x49, 0xB9, 0x64, 0xEF,
        0xA7, 0x82, 0xB5, 0xCF, 0x6D, 0x70, 0x13, 0xB0, 0x0F, 0x85, 0xF6, 0xA9, 0x08, 0xAA, 0x4D, 0x67,
        0x66, 0x87, 0xFA, 0x89, 0xFF, 0x75, 0x90, 0x18, 0x1E, 0x6B, 0x3D, 0xE9, 0x8A, 0x68, 0xC9, 0x26,
        0x04, 0xD9, 0x80, 0xCE, 0x3F, 0x5E, 0x92, 0xCE, 0x01, 0xFF, 0x06, 0x3B, 0xF2, 0xC1, 0xA9, 0x0C,
        0xCE, 0x02, 0x6F, 0x16, 0xBC, 0x92, 0x42, 0x0A, 0x41, 0x64, 0xCD, 0x52, 0xB6, 0x34, 0x4D, 0xAE,
        0xC0, 0x2E, 0xDE, 0xA4, 0xDF, 0x27, 0x68, 0x3C, 0xC1, 0xA0, 0x60, 0xAD, 0x43, 0xF3, 0xFC, 0x86,
        0xC1, 0x3E, 0x6C, 0x46, 0xF7, 0x7C, 0x29, 0x9F, 0xFA, 0xFD, 0xF0, 0xE3, 0xCE, 0x64, 0xE7, 0x35,
        0xF2, 0xF6, 0x56, 0x56, 0x6F, 0x6D, 0xF1, 0xE2, 0x42, 0xB0, 0x83, 0x40, 0xA5, 0xC3, 0x20, 0x2B,
        0xCC, 0x9A, 0xAE, 0xCA, 0xED, 0x4D, 0x70, 0x30, 0xA8, 0x70, 0x1C, 0x70, 0xFD, 0x13, 0x63, 0x29,
        0x02, 0x79, 0xEA, 0xD2, 0xA7, 0xAF, 0x35, 0x28, 0x32, 0x1C, 0x7B, 0xE6, 0x2F, 0x1A, 0xAA, 0x40,
        0x7E, 0x32, 0x8C, 0x27, 0x42, 0xFE, 0x82, 0x78, 0xEC, 0x0D, 0xEB, 0xE6, 0x83, 0x4B, 0x6D, 0x81,
        0x04, 0x40, 0x1A, 0x9E, 0x9A, 0x67, 0xF6, 0x72, 0x29, 0xFA, 0x04, 0xF0, 0x9D, 0xE4, 0xF4, 0x03,
    },
    {
        0xAD, 0xE3, 0xE1, 0xFA, 0x04, 0x35, 0xE5, 0xB6, 0xDD, 0x49, 0xEA, 0x89, 0x29, 0xB1, 0xFF, 0xB6,
        0x43, 0xDF, 0xCA, 0x96, 0xA0, 0x4A, 0x13, 0xDF, 0x43, 0xD9, 0x94, 0x97, 0x96, 0x43, 0x65, 0x48,
        0x70, 0x58, 0x33, 0xA2, 0x7D, 0x35, 0x7B, 0x96, 0x74, 0x5E, 0x0B, 0x5C, 0x32, 0x18, 0x14, 0x24,
        0xC2, 0x58, 0xB3, 0x6C, 0x22, 0x7A, 0xA1, 0xB7, 0xCB, 0x90, 0xA7, 0xA3, 0xF9, 0x7D, 0x45, 0x16,
        0xA5, 0xC8, 0xED, 0x8F, 0xAD, 0x39, 0x5E, 0x9E, 0x4B, 0x51, 0x68, 0x7D, 0xF8, 0x0C, 0x35, 0xC6,
        0x3F, 0x91, 0xAE, 0x44, 0xA5, 0x92, 0x30, 0x0D, 0x46, 0xF8, 0x40, 0xFF, 0xD0, 0xFF, 0x06, 0xD2,
        0x1C, 0x7F, 0x96, 0x18, 0xDC, 0xB7, 0x1D, 0x66, 0x3E, 0xD1, 0x73, 0xBC, 0x15, 0x8A, 0x2F, 0x94,
        0xF3, 0x00, 0xC1, 0x83, 0xF1, 0xCD, 0xD7, 0x81, 0x88, 0xAB, 0xDF, 0x8C, 0xEF, 0x97, 0xDD, 0x1B,
        0x17, 0x5F, 0x58, 0xF6, 0x9A, 0xE9, 0xE8, 0xC2, 0x2F, 0x38, 0x15, 0xF5, 0x21, 0x07, 0xF8, 0x37,
        0x90, 0x5D, 0x2E, 0x02, 0x40, 0x24, 0x15, 0x0D, 0x25, 0xB7, 0x26, 0x5D, 0x09, 0xCC, 0x4C, 0xF4,
        0xF2, 0x1B, 0x94, 0x70, 0x5A, 0x9E, 0xEE, 0xED, 0x77, 0x77, 0xD4, 0x51, 0x99, 0xF5, 0xDC, 0x76,
        0x1E, 0xE3, 0x6C, 0x8C, 0xD1, 0x12, 0xD4, 0x57, 0xD1, 0xB6, 0x83, 0xE4, 0xE4, 0xFE, 0xDA, 0xE9,
        0xB4, 0x3B, 0x33, 0xE5, 0x37, 0x8A, 0xDF, 0xB5, 0x7F, 0x89, 0xF1, 0x9B, 0x9E, 0xB0, 0x15, 0xB2,
        0x3A, 0xFE, 0xEA, 0x61, 0x84, 0x5B, 0x7D, 0x4B, 0x23, 0x12, 0x0B, 0x83, 0x12, 0xF2, 0x22, 0x6B,
        0xB9, 0x22, 0x96, 0x4B, 0x26, 0x0B, 0x63, 0x5E, 0x96, 0x57, 0x52, 0xA3, 0x67, 0x64, 0x22, 0xCA,
        0xD0, 0x56, 0x3E, 0x74, 0xB5, 0x98, 0x1F, 0x0D, 0xF8, 0xB3, 0x34, 0xE6, 0x98, 0x68, 0x5A, 0xAD,
    }
};

const unsigned char acid_fixed_key_moduli_retail[0x2][0x100] = { /* Fixed RSA keys used to validate ACID signatures. */
    {
        0xDD, 0xC8, 0xDD, 0xF2, 0x4E, 0x6D, 0xF0, 0xCA, 0x9E, 0xC7, 0x5D, 0xC7, 0x7B, 0xAD, 0xFE, 0x7D,
        0x23, 0x89, 0x69, 0xB6, 0xF2, 0x06, 0xA2, 0x02, 0x88, 0xE1, 0x55, 0x91, 0xAB, 0xCB, 0x4D, 0x50,
        0x2E, 0xFC, 0x9D, 0x94, 0x76, 0xD6, 0x4C, 0xD8, 0xFF, 0x10, 0xFA, 0x5E, 0x93, 0x0A, 0xB4, 0x57,
        0xAC, 0x51, 0xC7, 0x16, 0x66, 0xF4, 0x1A, 0x54, 0xC2, 0xC5, 0x04, 0x3D, 0x1B, 0xFE, 0x30, 0x20,
        0x8A, 0xAC, 0x6F, 0x6F, 0xF5, 0xC7, 0xB6, 0x68, 0xB8, 0xC9, 0x40, 0x6B, 0x42, 0xAD, 0x11, 0x21,
        0xE7, 0x8B, 0xE9, 0x75, 0x01, 0x86, 0xE4, 0x48, 0x9B, 0x0A, 0x0A, 0xF8, 0x7F, 0xE8, 0x87, 0xF2,
        0x82, 0x01, 0xE6, 0xA3, 0x0F, 0xE4, 0x66, 0xAE, 0x83, 0x3F, 0x4E, 0x9F, 0x5E, 0x01, 0x30, 0xA4,
        0x00, 0xB9, 0x9A, 0xAE, 0x5F, 0x03, 0xCC, 0x18, 0x60, 0xE5, 0xEF, 0x3B, 0x5E, 0x15, 0x16, 0xFE,
        0x1C, 0x82, 0x78, 0xB5, 0x2F, 0x47, 0x7C, 0x06, 0x66, 0x88, 0x5D, 0x35, 0xA2, 0x67, 0x20, 0x10,
        0xE7, 0x6C, 0x43, 0x68, 0xD3, 0xE4, 0x5A, 0x68, 0x2A, 0x5A, 0xE2, 0x6D, 0x73, 0xB0, 0x31, 0x53,
        0x1C, 0x20, 0x09, 0x44, 0xF5, 0x1A, 0x9D, 0x22, 0xBE, 0x12, 0xA1, 0x77, 0x11, 0xE2, 0xA1, 0xCD,
        0x40, 0x9A, 0xA2, 0x8B, 0x60, 0x9B, 0xEF, 0xA0, 0xD3, 0x48, 0x63, 0xA2, 0xF8, 0xA3, 0x2C, 0x08,
        0x56, 0x52, 0x2E, 0x60, 0x19, 0x67, 0x5A, 0xA7, 0x9F, 0xDC, 0x3F, 0x3F, 0x69, 0x2B, 0x31, 0x6A,
        0xB7, 0x88, 0x4A, 0x14, 0x84, 0x80, 0x33, 0x3C, 0x9D, 0x44, 0xB7, 0x3F, 0x4C, 0xE1, 0x75, 0xEA,
        0x37, 0xEA, 0xE8, 0x1E, 0x7C, 0x77, 0xB7, 0xC6, 0x1A, 0xA2, 0xF0, 0x9F, 0x10, 0x61, 0xCD, 0x7B,
        0x5B, 0x32, 0x4C, 0x37, 0xEF, 0xB1, 0x71, 0x68, 0x53, 0x0A, 0xED, 0x51, 0x7D, 0x35, 0x22, 0xFD,
    },
    {
        0xE7, 0xAA, 0x25, 0xC8, 0x01, 0xA5, 0x14, 0x6B, 0x01, 0x60, 0x3E, 0xD9, 0x96, 0x5A, 0xBF, 0x90,
        0xAC, 0xA7, 0xFD, 0x9B, 0x5B, 0xBD, 0x8A, 0x26, 0xB0, 0xCB, 0x20, 0x28, 0x9A, 0x72, 0x12, 0xF5,
        0x20, 0x65, 0xB3, 0xB9, 0x84, 0x58, 0x1F, 0x27, 0xBC, 0x7C, 0xA2, 0xC9, 0x9E, 0x18, 0x95, 0xCF,
        0xC2, 0x73, 0x2E, 0x74, 0x8C, 0x66, 0xE5, 0x9E, 0x79, 0x2B, 0xB8, 0x07, 0x0C, 0xB0, 0x4E, 0x8E,
        0xAB, 0x85, 0x21, 0x42, 0xC4, 0xC5, 0x6D, 0x88, 0x9C, 0xDB, 0x15, 0x95, 0x3F, 0x80, 0xDB, 0x7A,
        0x9A, 0x7D, 0x41, 0x56, 0x25, 0x17, 0x18, 0x42, 0x4D, 0x8C, 0xAC, 0xA5, 0x7B, 0xDB, 0x42, 0x5D,
        0x59, 0x35, 0x45, 0x5D, 0x8A, 0x02, 0xB5, 0x70, 0xC0, 0x72, 0x35, 0x46, 0xD0, 0x1D, 0x60, 0x01,
        0x4A, 0xCC, 0x1C, 0x46, 0xD3, 0xD6, 0x35, 0x52, 0xD6, 0xE1, 0xF8, 0x3B, 0x5D, 0xEA, 0xDD, 0xB8,
        0xFE, 0x7D, 0x50, 0xCB, 0x35, 0x23, 0x67, 0x8B, 0xB6, 0xE4, 0x74, 0xD2, 0x60, 0xFC, 0xFD, 0x43,
        0xBF, 0x91, 0x08, 0x81, 0xC5, 0x4F, 0x5D, 0x16, 0x9A, 0xC4, 0x9A, 0xC6, 0xF6, 0xF3, 0xE1, 0xF6,
        0x5C, 0x07, 0xAA, 0x71, 0x6C, 0x13, 0xA4, 0xB1, 0xB3, 0x66, 0xBF, 0x90, 0x4C, 0x3D, 0xA2, 0xC4,
        0x0B, 0xB8, 0x3D, 0x7A, 0x8C, 0x19, 0xFA, 0xFF, 0x6B, 0xB9, 0x1F, 0x02, 0xCC, 0xB6, 0xD3, 0x0C,
        0x7D, 0x19, 0x1F, 0x47, 0xF9, 0xC7, 0x40, 0x01, 0xFA, 0x46, 0xEA, 0x0B, 0xD4, 0x02, 0xE0, 0x3D,
        0x30, 0x9A, 0x1A, 0x0F, 0xEA, 0xA7, 0x66, 0x55, 0xF7, 0xCB, 0x28, 0xE2, 0xBB, 0x99, 0xE4, 0x83,
        0xC3, 0x43, 0x03, 0xEE, 0xDC, 0x1F, 0x02, 0x23, 0xDD, 0xD1, 0x2D, 0x39, 0xA4, 0x65, 0x75, 0x03,
        0xEF, 0x37, 0x9C, 0x06, 0xD6, 0xFA, 0xA1, 0x15, 0xF0, 0xDB, 0x17, 0x47, 0x26, 0x4F, 0x49, 0x03
    }
};

} // namespace

auto GetContentTypeStr(u8 content_type) -> const char* {
    switch (content_type) {
        case ContentType_Program: return "Program";
        case ContentType_Meta: return "Meta";
        case ContentType_Control: return "Control";
        case ContentType_Manual: return "Manual";
        case ContentType_Data: return "Data";
        case ContentType_PublicData: return "PublicData";
    }

    return "Unknown";
}

auto GetDistributionTypeStr(u8 distribution_type) -> const char* {
    switch (distribution_type) {
        case DistributionType_System: return "System";
        case DistributionType_GameCard: return "GameCard";
    }

    return "Unknown";
}

Result DecryptHeader(const void* in, const keys::Keys& keys, Header& out) {
    crypto::cryptoAes128Xts(in, &out, keys.header_key, 0, 0x200, sizeof(out), false);
    R_UNLESS(out.IsValid(), Result_NcaBadMagic);
    R_SUCCEED();
}

Result DecryptKeak(const keys::Keys& keys, Header& header) {
    const auto key_generation = header.GetKeyGeneration();

    // try with spl.
    keys::KeyEntry keak;
    if (R_SUCCEEDED(splCryptoGenerateAesKek(g_key_area_key[header.kaek_index], key_generation, 0, &keak))) {
        for (auto& key_area : header.key_area) {
            R_TRY(splCryptoGenerateAesKey(&keak, std::addressof(key_area), std::addressof(key_area)));
        }
    } else {
        // failed with spl, try using keys.
        R_TRY(keys.GetNcaKeyArea(&keak, key_generation, header.kaek_index));
        for (auto& key_area : header.key_area) {
            crypto::cryptoAes128(std::addressof(key_area), std::addressof(key_area), std::addressof(keak), false);
        }
    }

    R_SUCCEED();
}

Result EncryptKeak(const keys::Keys& keys, Header& header, u8 key_generation) {
    header.SetKeyGeneration(key_generation);

    keys::KeyEntry keak;
    R_TRY(keys.GetNcaKeyArea(&keak, key_generation, header.kaek_index));
    log_write("re-encrypting with: 0x%X\n", key_generation);

    for (auto& key_area : header.key_area) {
        crypto::cryptoAes128(std::addressof(key_area), std::addressof(key_area), std::addressof(keak), true);
    }

    R_SUCCEED();
}

Result VerifyFixedKey(const Header& header) {
    R_UNLESS(header.sig_key_gen < std::size(nca_hdr_fixed_key_moduli_retail), Result_NcaBadSigKeyGen);
    auto mod = nca_hdr_fixed_key_moduli_retail[header.sig_key_gen];

    const u8 E[3] = { 1, 0, 1 };
    if (!rsa2048VerifySha256BasedPssSignature(&header.magic, 0x200, header.rsa_fixed_key, mod, E, sizeof(E))) {
        auto new_header = header;
        // if failed, detect if this is a eshop/xci convert.
        new_header.distribution_type ^= 1;
        if (!rsa2048VerifySha256BasedPssSignature(&new_header.magic, 0x200, new_header.rsa_fixed_key, mod, E, sizeof(E))) {
            log_write("FAILED nca header hash\n");
            R_THROW(Result_NcaFailedNcaHeaderHashVerify);
        } else {
            log_write("WARNING! nca is converted! distribution_type: %u\n", new_header.distribution_type);
            R_SUCCEED();
        }
    }

    R_SUCCEED();
}

Result ParseCnmt(const fs::FsPath& path, u64 program_id, ncm::PackagedContentMeta& header, std::vector<u8>& extended_header, std::vector<NcmPackagedContentInfo>& infos) {
    fs::FsNativeId fs{program_id, FsFileSystemType_ContentMeta, path, FsContentAttributes_All};
    R_TRY(fs.GetFsOpenResult());

    fs::Dir dir;
    R_TRY(fs.OpenDirectory("/", FsDirOpenMode_ReadFiles, &dir));

    s64 total_entries;
    FsDirectoryEntry buf;
    R_TRY(dir.Read(std::addressof(total_entries), 1, std::addressof(buf)));

    fs::File file;
    R_TRY(fs.OpenFile(fs::AppendPath("/", buf.name), FsOpenMode_Read, &file));

    s64 offset{};
    u64 bytes_read;
    R_TRY(file.Read(offset, std::addressof(header), sizeof(header), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    // read extended header
    extended_header.resize(header.meta_header.extended_header_size);
    R_TRY(file.Read(offset, extended_header.data(), extended_header.size(), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    // read infos.
    infos.resize(header.meta_header.content_count);
    R_TRY(file.Read(offset, infos.data(), infos.size() * sizeof(NcmPackagedContentInfo), 0, std::addressof(bytes_read)));
    offset += bytes_read;

    R_SUCCEED();
}

Result ParseControl(const fs::FsPath& path, u64 program_id, void* nacp_out, s64 nacp_size, std::vector<u8>* icon_out, s64 nacp_off) {
    fs::FsNativeId fs{program_id, FsFileSystemType_ContentControl, path, FsContentAttributes_All};
    R_TRY(fs.GetFsOpenResult());

    // read nacp.
    if (nacp_out) {
        fs::File file;
        R_TRY(fs.OpenFile("/control.nacp", FsOpenMode_Read, &file));

        u64 bytes_read;
        if (nacp_off == 0) {
            NacpStruct nacp;
            R_TRY(file.Read(0, &nacp, sizeof(NacpStruct), 0, &bytes_read));

            // NULL on alloc failure or corrupt zlib stream, keep the raw nacp and let NormalizeNacpLangData() retry.
            if (auto* lang_entries = nxtcDecompressNacpTitleBlock(&nacp)) {
                ON_SCOPE_EXIT(std::free(lang_entries));
                std::memcpy(&nacp, lang_entries, sizeof(NacpLanguageEntryData));
                // mark as decompressed so NormalizeNacpLangData() and nxtcAddEntry() don't inflate it again.
                nacp.titles_data_format = 0;
            } else {
                log_write("[NACP] failed to decompress title block for %016lX\n", program_id);
            }

            std::memcpy(nacp_out, &nacp, nacp_size);
        } else {
            R_TRY(file.Read(nacp_off, nacp_out, nacp_size, 0, &bytes_read));
        }
    }

    // read icon.
    if (icon_out) {
        static bool checked_lang = false;
        static SetLanguage setLanguage = SetLanguage_ENUS;

        if (!checked_lang) {
            checked_lang = true;
            u64 languageCode;
            if (R_SUCCEEDED(setGetSystemLanguage(&languageCode))) {
                setMakeLanguage(languageCode, &setLanguage);
            }
        }

        static const char* icon_names[SetLanguage_Total] = {
            [SetLanguage_JA] = "icon_Japanese.dat",
            [SetLanguage_ENUS] = "icon_AmericanEnglish.dat",
            [SetLanguage_FR] = "icon_French.dat",
            [SetLanguage_DE] = "icon_German.dat",
            [SetLanguage_IT] = "icon_Italian.dat",
            [SetLanguage_ES] = "icon_Spanish.dat",
            [SetLanguage_ZHCN] = "icon_SimplifiedChinese.dat",
            [SetLanguage_KO] = "icon_Korean.dat",
            [SetLanguage_NL] = "icon_Dutch.dat",
            [SetLanguage_PT] = "icon_Portuguese.dat",
            [SetLanguage_RU] = "icon_Russian.dat",
            [SetLanguage_ZHTW] = "icon_TraditionalChinese.dat",
            [SetLanguage_ENGB] = "icon_BritishEnglish.dat",
            [SetLanguage_FRCA] = "icon_CanadianFrench.dat",
            [SetLanguage_ES419] = "icon_LatinAmericanSpanish.dat",
            [SetLanguage_ZHHANS] = "icon_SimplifiedChinese.dat",
            [SetLanguage_ZHHANT] = "icon_TraditionalChinese.dat",
            [SetLanguage_PTBR] = "icon_BrazilianPortuguese.dat",
        };

        // try and open the icon for the specific langauge.
        fs::File file;
        const auto file_name = icon_names[setLanguage];
        if (!std::strlen(file_name) || R_FAILED(fs.OpenFile(fs::AppendPath("/", file_name), FsOpenMode_Read, &file))) {
            // otherwise, fallback to US icon.
            R_TRY(fs.OpenFile(fs::AppendPath("/", icon_names[SetLanguage_ENUS]), FsOpenMode_Read, &file));
        }

        s64 size;
        R_TRY(file.GetSize(&size));
        icon_out->resize(size);

        u64 bytes_read;
        R_TRY(file.Read(0, icon_out->data(), icon_out->size(), 0, &bytes_read));
    }

    R_SUCCEED();
}

auto GetKeyGenStr(u8 key_gen) -> const char* {
    switch (key_gen) {
        case KeyGenerationOld_100: return "1.0.0";
        case KeyGenerationOld_300: return "3.0.0";
        case KeyGeneration_301: return "3.0.1";
        case KeyGeneration_400: return "4.0.0";
        case KeyGeneration_500: return "5.0.0";
        case KeyGeneration_600: return "6.0.0";
        case KeyGeneration_620: return "6.2.0";
        case KeyGeneration_700: return "7.0.0";
        case KeyGeneration_810: return "8.1.0";
        case KeyGeneration_900: return "9.0.0";
        case KeyGeneration_910: return "9.1.0";
        case KeyGeneration_1210: return "12.1.0";
        case KeyGeneration_1300: return "13.0.0";
        case KeyGeneration_1400: return "14.0.0";
        case KeyGeneration_1500: return "15.0.0";
        case KeyGeneration_1600: return "16.0.0";
        case KeyGeneration_1700: return "17.0.0";
        case KeyGeneration_1800: return "18.0.0";
        case KeyGeneration_1900: return "19.0.0";
        case KeyGeneration_2000: return "20.0.0";
        case KeyGeneration_2100: return "21.0.0";
        case KeyGeneration_2200: return "22.0.0";
        case KeyGeneration_2300: return "23.0.0";
    }

    return "Unknown";
}

Result GetDecryptedTitleKey(Header& header, const keys::Keys& keys, keys::KeyEntry& out) {
    return GetDecryptedTitleKey(nullptr, "", header, keys, out);
}

Result GetDecryptedTitleKey(fs::Fs* fs, const fs::FsPath& path, Header& header, const keys::Keys& keys, keys::KeyEntry& out) {
    if (es::IsRightsIdValid(header.rights_id)) {
        R_TRY(es::Initialize());
        ON_SCOPE_EXIT(es::Exit());

        bool got_key = false;
        if (fs) {
            // try and get ticket from path as it may not be installed yet.
            fs::FsPath tik_path = path;
            if (auto dilem = std::strrchr(tik_path, '/')) {
                std::sprintf(dilem, "/%s.tik", utils::hexIdToStr(header.rights_id).str);
                log_write("[NCA] trying to read local ticket file: %s\n", tik_path.s);
                std::vector<u8> tik_data;
                if (R_SUCCEEDED(fs->read_entire_file(tik_path, tik_data))) {
                    log_write("[NCA] read local ticket file: %s\n", tik_path.s);
                    if (R_SUCCEEDED(es::GetTitleKeyDecrypted(tik_data, header.rights_id, header.GetKeyGeneration(), keys, out))) {
                        log_write("[NCA] decrypted local ticket file: %s\n", tik_path.s);
                        got_key = true;
                    }
                }
            }
        }

        if (!got_key) {
            R_TRY(es::GetTitleKeyDecrypted(header.rights_id, header.GetKeyGeneration(), keys, out));
        }
    } else {
        // todo: ignore empty key areas (sphaira forwarders)
        R_TRY(nca::DecryptKeak(keys, header));
        std::memcpy(out.key, &header.key_area[0x2], sizeof(out));
    }

    R_SUCCEED();
}

DecyptedData::DecyptedData(u64 align, const std::shared_ptr<yati::source::Base>& source)
: m_source{source}
, m_align{align} {
}

Result DecyptedData::Read(void *_buf, s64 _off, s64 _size, u64* _bytes_read) {
    const auto aligned_off = utils::AlignDown<s64>(_off, m_align);
    auto aligned_size = utils::AlignUp<s64>(_size, m_align);
    if (aligned_off != _off) {
        aligned_size += m_align;
    }

    u8* buf = (u8*)_buf;
    std::unique_ptr<u8[]> aligned_buf{};
    const auto use_align = _off != aligned_off || _size != aligned_size;

    if (use_align) {
        aligned_buf = std::make_unique_for_overwrite<u8[]>(aligned_size);
        buf = aligned_buf.get();
    }

    u64 bytes_read;
    R_TRY(m_source->Read(buf, aligned_off, aligned_size, &bytes_read));
    R_UNLESS(bytes_read == aligned_size, 18);

    R_TRY(Decrypt(buf, aligned_off, aligned_size));

    if (use_align) {
        std::memcpy(_buf, buf + (_off - aligned_off), _size);
    }

    *_bytes_read = _size;
    R_SUCCEED();
}

DecyptedDataCtr::DecyptedDataCtr(const void* key, u64 ctr, const std::shared_ptr<yati::source::Base>& source)
: DecyptedData{AES_BLOCK_SIZE, source} {
    SetCtr(ctr);
    aes128CtrContextCreate(&m_ctx, key, m_ctr);
}

Result DecyptedDataCtr::SetCtr(u64 ctr) {
    crypto::SetCtr(m_ctr, ctr);
    R_SUCCEED();
}

Result DecyptedDataCtr::Decrypt(void* buf, s64 off, s64 size) {
    crypto::UpdateCtr(m_ctr, off);
    aes128CtrContextResetCtr(&m_ctx, m_ctr);
    aes128CtrCrypt(&m_ctx, buf, buf, size);
    R_SUCCEED();
}

NcaReader::NcaReader(const nca::Header& decrypted_header, const void* key, u64 size, const std::shared_ptr<yati::source::Base>& source)
: m_header{decrypted_header}
, m_capacity{size}
, m_source{source} {
    m_decryptor = std::make_unique<DecyptedDataCtr>(key, 0, source);
    std::memcpy(m_key, key, sizeof(m_key));
}

Result NcaReader::Read(void *_buf, s64 off, s64 size, u64* bytes_read) {
    return ReadInternal(_buf, off, size, bytes_read, true);
}

Result NcaReader::ReadEncrypted(void *_buf, s64 off, s64 size, u64* bytes_read) {
    return ReadInternal(_buf, off, size, bytes_read, false);
}

Result NcaReader::ReadInternal(void *_buf, s64 off, s64 size, u64* bytes_read_out, bool decrypt) {
    *bytes_read_out = 0;

    R_UNLESS(m_header.IsValid(), Result_YatiInvalidNcaMagic);
    R_UNLESS(off < m_capacity, FsError_UnsupportedOperateRangeForFileStorage);
    size = std::min<s64>(size, m_capacity - off);
    u8* buf = (u8*)_buf;

    while (size) {
        u64 ctr = 0;
        bool encrypted = false;
        auto rsize = size;

        if (decrypt) {
            // try and find a section.
            for (u32 i = 0; i < m_header.GetSectionCount(); i++) {
                const auto& fs_header = m_header.fs_header[i];
                const auto& fs_table = m_header.fs_table[i];

                // check if this is the section we want.
                if (off >= fs_table.GetOffset() && off < fs_table.GetOffsetEnd()) {
                    rsize = std::min<s64>(rsize, fs_table.GetOffsetEnd() - off);

                    // check if this is compressed.
                    if (fs_header.encryption_type >= nca::EncryptionType::EncryptionType_AesCtr) {
                        ctr = fs_header.section_ctr;
                        encrypted = true;
                    }

                    break;
                }
            }
        }

        u64 bytes_read;
        if (encrypted) {
            R_TRY(m_decryptor->SetCtr(ctr));
            R_TRY(m_decryptor->Read(buf, off, rsize, &bytes_read));
        } else {
            R_TRY(m_source->Read(buf, off, rsize, &bytes_read));
        }

        R_UNLESS(bytes_read == rsize, 16);

        size -= rsize;
        off += rsize;
        buf += rsize;
        *bytes_read_out += rsize;
    }

    R_SUCCEED();
}

} // namespace sphaira::nca
