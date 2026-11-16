#pragma once

#include "ncm.hpp"

#include <switch.h>
#include <span>
#include <vector>

namespace sphaira::ns {

enum ApplicationRecordType {
    ApplicationRecordType_Running         = 0x0,
    ApplicationRecordType_Installed       = 0x3,
    ApplicationRecordType_Downloading     = 0x4,
    // application is gamecard, but gamecard isn't insterted
    ApplicationRecordType_GamecardMissing = 0x5,
    ApplicationRecordType_Downloaded      = 0x6,
    ApplicationRecordType_Updated         = 0xA,
    ApplicationRecordType_Archived        = 0xB,
};

Result Initialize();
void Exit();

Result PushApplicationRecord(u64 tid, const ncm::ContentStorageRecord* records, u32 count);
Result ListApplicationRecordContentMeta(u64 offset, u64 tid, ncm::ContentStorageRecord* out_records, u32 count, s32* entries_read);
Result DeleteApplicationRecord(u64 tid);
Result InvalidateApplicationControlCache(u64 tid);

// nsGetApplicationControlData() refuses most Switch 2 titles (0x5010), on
// Switch 2 the newer command answers for them. checks that actual_size is valid.
Result GetApplicationControlData(NsApplicationControlSource source, u64 app_id, NsApplicationControlData* data, u64* actual_size);

// helpers

// fills out with the number or records available
Result GetApplicationRecords(u64 id, std::vector<ncm::ContentStorageRecord>& out);

// sets the lowest launch version based on the current record list.
Result SetLowestLaunchVersion(u64 id);
// same as above, but uses the provided record list.
Result SetLowestLaunchVersion(u64 id, std::span<const ncm::ContentStorageRecord> records);

static inline bool IsNsControlFetchSlow() {
    return hosversionAtLeast(20,0,0);
}

} // namespace sphaira::ns
