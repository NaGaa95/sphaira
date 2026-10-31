#pragma once

#include <switch.h>
#include "defines.hpp"

namespace sphaira::usb::api {

enum : u32 {
    MAGIC = 0x53504830,
    PACKET_SIZE = 24,
};

enum : u32 {
    CMD_QUIT = 0,
    CMD_OPEN = 1,
    CMD_EXPORT = 1,
};

enum : u32 {
    RESULT_OK = 0,
    RESULT_ERROR = 1,
};

enum : u32 {
    FLAG_NONE = 0,
    FLAG_STREAM = 1 << 0,
};

struct UsbPacket {
    u32 magic{};
    u32 arg2{};
    u32 arg3{};
    u32 arg4{};
    u32 arg5{};
    u32 crc32c{}; // crc32 over the above 16 bytes.

protected:
    u32 CalculateCrc32c() const {
        return crc32cCalculate(this, 20);
    }

    void GenerateCrc32c() {
        crc32c = CalculateCrc32c();
    }

    Result Verify() const {
        R_UNLESS(crc32c == CalculateCrc32c(), Result_UsbBadPacketCrc);
        R_UNLESS(magic == MAGIC, Result_UsbBadMagic);
        R_SUCCEED();
    }
};

struct SendPacket : UsbPacket {
    static SendPacket Build(u32 cmd, u32 arg3 = 0, u32 arg4 = 0) {
        SendPacket packet{MAGIC, cmd, arg3, arg4};
        packet.GenerateCrc32c();
        return packet;
    }

    Result Verify() const {
        return UsbPacket::Verify();
    }

    u32 GetCmd() const {
        return arg2;
    }
};

struct ResultPacket : UsbPacket {
    static ResultPacket Build(u32 result, u32 arg3 = 0, u32 arg4 = 0) {
        ResultPacket packet{MAGIC, result, arg3, arg4};
        packet.GenerateCrc32c();
        return packet;
    }

    Result Verify() const {
        R_TRY(UsbPacket::Verify());
        R_UNLESS(arg2 == RESULT_OK, Result_UsbRemoteError);
        R_SUCCEED();
    }
};

struct SendDataPacket : UsbPacket {
    static SendDataPacket Build(u64 off, u32 size, u32 crc32c) {
        SendDataPacket packet{MAGIC, u32(off >> 32), u32(off), size, crc32c};
        packet.GenerateCrc32c();
        return packet;
    }

    Result Verify() const {
        return UsbPacket::Verify();
    }

    u64 GetOffset() const {
        return (u64(arg2) << 32) | arg3;
    }

    u32 GetSize() const {
        return arg4;
    }

    u32 GetCrc32c() const {
        return arg5;
    }
};

static_assert(sizeof(UsbPacket) == PACKET_SIZE);
static_assert(sizeof(SendPacket) == PACKET_SIZE);
static_assert(sizeof(ResultPacket) == PACKET_SIZE);
static_assert(sizeof(SendDataPacket) == PACKET_SIZE);

} // namespace sphaira::usb::api
