#pragma once

#include "base.hpp"
#include <string>
#include <memory>
#include <stop_token>
#include <curl/curl.h>
#include <switch.h>

namespace sphaira::devoptab::common {
struct PushThreadData;
} // namespace sphaira::devoptab::common

namespace sphaira::yati::source {

// a file served over http, read with range requests so it installs through the
// random access path and skipped content is never downloaded. one transfer runs
// for as long as reads follow on; a jump, or an early end, starts another.
struct Http final : Base {
    // `user` empty sends no credentials; otherwise they go as basic auth on the
    // first request, rather than after the server has challenged for them.
    Http(const std::string& url, const std::string& user, const std::string& pass, std::stop_token token);
    ~Http();

    Result Read(void* buf, s64 off, s64 size, u64* bytes_read) override;

private:
    Result Start(s64 off);

    const std::string m_url;
    const std::string m_user;
    const std::string m_pass;
    const std::stop_token m_token;
    CURL* m_curl{};
    std::unique_ptr<devoptab::common::PushThreadData> m_transfer{};
    // where the running transfer has read up to.
    s64 m_offset{};
};

} // namespace sphaira::yati::source
