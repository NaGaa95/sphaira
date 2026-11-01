#include "usb/usb_installer.hpp"
#include "usb/usb_api.hpp"
#include "defines.hpp"
#include "log.hpp"

#include <ranges>

namespace sphaira::usb::install {
namespace {

using namespace usb::api;

} // namespace

Usb::Usb(u64 transfer_timeout) {
    m_usb = std::make_unique<usb::UsbDs>(transfer_timeout);
    m_open_result = m_usb->Init();
}

Usb::~Usb() {
    if (m_was_connected && R_SUCCEEDED(m_usb->IsUsbConnected(0))) {
        const auto send_header = SendPacket::Build(CMD_QUIT);
        SendAndVerify(&send_header, sizeof(send_header), 1e+9);
    }
}

Result Usb::IsUsbConnected(u64 timeout) {
    return m_usb->IsUsbConnected(timeout);
}

Result Usb::WaitForConnection(u64 timeout, std::vector<std::string>& out_names) {
    m_was_connected = false;

    // ensure that we are connected.
    R_TRY(m_open_result);
    R_TRY(m_usb->IsUsbConnected(timeout));

    const auto send_header = SendPacket::Build(RESULT_OK);
    ResultPacket recv_header;
    R_TRY(SendAndVerify(&send_header, sizeof(send_header), timeout, &recv_header))

    std::vector<char> names(recv_header.arg3);
    R_TRY(m_usb->TransferAll(true, names.data(), names.size(), timeout));

    out_names.clear();
    for (const auto& name : std::views::split(names, '\n')) {
        if (!name.empty()) {
            auto& it = out_names.emplace_back(name.data(), name.size());
            log_write("[USB] got name: %s\n", it.c_str());
        }
    }

    m_was_connected = true;
    R_SUCCEED();
}

Result Usb::OpenFile(u32 index, s64& file_size) {
    log_write("doing open file\n");
    const auto send_header = SendPacket::Build(CMD_OPEN, index);
    ResultPacket recv_header;
    R_TRY(SendAndVerify(&send_header, sizeof(send_header), &recv_header))
    log_write("did open file\n");

    const auto flags = recv_header.arg3 >> 16;
    const auto file_size_msb = recv_header.arg3 & 0xFFFF;
    const auto file_size_lsb = recv_header.arg4;

    m_flags = flags;
    file_size = ((u64)file_size_msb << 32) | file_size_lsb;
    R_SUCCEED();
}

Result Usb::CloseFile() {
    const auto send_header = SendDataPacket::Build(0, 0, 0);

    return SendAndVerify(&send_header, sizeof(send_header));
}

void Usb::SignalCancel() {
    m_usb->Cancel();
}

u32 Usb::GetFlags() const {
    return m_flags;
}

Result Usb::Read(void* buf, u64 off, u32 size, u64* bytes_read) {
    const auto send_header = SendDataPacket::Build(off, size, 0);
    ResultPacket recv_header;
    R_TRY(SendAndVerify(&send_header, sizeof(send_header), &recv_header))

    // adjust the size and read the data.
    size = recv_header.arg3;
    R_TRY(m_usb->TransferAll(true, buf, size));

    // verify crc32c.
    R_UNLESS(crc32cCalculate(buf, size) == recv_header.arg4, Result_UsbBadDataCrc);

    *bytes_read = size;
    R_SUCCEED();
}

// casts away const, but it does not modify the buffer!
Result Usb::SendAndVerify(const void* data, u32 size, u64 timeout, ResultPacket* out) {
    R_TRY(m_usb->TransferAll(false, const_cast<void*>(data), size, timeout));

    ResultPacket recv_header;
    R_TRY(m_usb->TransferAll(true, &recv_header, sizeof(recv_header), timeout));
    R_TRY(recv_header.Verify());

    if (out) *out = recv_header;
    R_SUCCEED();
}

Result Usb::SendAndVerify(const void* data, u32 size, ResultPacket* out) {
    return SendAndVerify(data, size, m_usb->GetTransferTimeout(), out);
}

} // namespace sphaira::usb::install
