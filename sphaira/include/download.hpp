#pragma once

#include "fs.hpp"
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include <algorithm>
#include <stop_token>
#include <switch.h>

namespace sphaira::curl {

// identifies sphaira in curl requests
constexpr auto USER_AGENT = "Sphaira/" APP_VERSION;

enum {
    Flag_None = 0,

    // requests to download send etag in the header.
    // the received etag is then saved on success.
    // this api is only available on downloading to file.
    Flag_Cache = 1 << 0,

    // sets CURLOPT_NOBODY.
    Flag_NoBody = 1 << 1,

    // clears CURLOPT_FAILONERROR, so an error response's body is delivered and a
    // server that explains itself can be quoted back. note the transfer then
    // counts as a success: the caller has to check result.code itself.
    Flag_KeepErrorBody = 1 << 2,
};

enum class Priority {
    Normal, // gets pushed to the back of the queue
    High, // gets pushed to the front of the queue
};

struct Api;
struct ApiResult;

using Path = fs::FsPath;
using OnComplete = std::function<void(ApiResult& result)>;
using OnProgress = std::function<bool(s64 dltotal, s64 dlnow, s64 ultotal, s64 ulnow)>;
using OnUploadCallback = std::function<size_t(void *ptr, size_t size)>;
using OnUploadSeek = std::function<bool(s64 offset)>;
using StopToken = std::stop_token;

struct Url {
    Url() = default;
    Url(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct Fields {
    Fields() = default;
    Fields(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct Header {
    Header() = default;
    Header(std::initializer_list<std::pair<const std::string, std::string>>&& p) : m_map{std::forward<decltype(p)>(p)} {}
    std::unordered_map<std::string, std::string> m_map;

    auto Find(const std::string& key) const {
        return std::find_if(m_map.cbegin(), m_map.cend(), [&key](auto& e) {
            return !strcasecmp(key.c_str(), e.first.c_str());
        });
    }
};

struct Flags {
    Flags() = default;
    Flags(u32 flags) : m_flags{flags} {}
    u32 m_flags{Flag_None};
};

struct Port {
    Port() = default;
    Port(u16 port) : m_port{port} {}
    u16 m_port{};
};

struct CustomRequest {
    CustomRequest() = default;
    CustomRequest(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct UserPass {
    UserPass() = default;
    UserPass(const std::string& user) : m_user{user} {}
    UserPass(const std::string& user, const std::string& pass) : m_user{user}, m_pass{pass} {}
    std::string m_user;
    std::string m_pass;
};

// sends the auth header up front instead of waiting for the server's 401:
// libcurl's CURLAUTH_ANY default always probes unauthenticated first. only has
// an effect alongside a non-empty UserPass.
struct PreemptiveAuth {
    PreemptiveAuth() = default;
    PreemptiveAuth(bool enable) : m_enable{enable} {}
    bool m_enable{};
};

struct UploadInfo {
    UploadInfo() = default;
    UploadInfo(const std::string& name) : m_name{name} {}
    UploadInfo(const std::string& name, s64 size, const OnUploadCallback& cb) : m_name{name}, m_size{size}, m_callback{cb} {}
    UploadInfo(const std::string& name, const std::vector<u8>& data) : m_name{name}, m_data{data} {}
    std::string m_name{};
    std::vector<u8> m_data{};
    s64 m_size{};
    OnUploadCallback m_callback{};
};

struct Bearer {
    Bearer() = default;
    Bearer(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct PubKey {
    PubKey() = default;
    PubKey(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct PrivKey {
    PrivKey() = default;
    PrivKey(const std::string& str) : m_str{str} {}
    std::string m_str;
};

struct ApiResult {
    bool success;
    long code;
    Header header; // returned headers in request
    std::vector<u8> data; // empty if downloaded a file
    fs::FsPath path; // empty if downloaded memory
};

struct DownloadEventData {
    OnComplete callback;
    ApiResult result;
    StopToken stoken;
};

// helper that generates the api using an location.
#define CURL_LOCATION_TO_API(loc) \
    curl::Url{loc.url}, \
    curl::UserPass{loc.user, loc.pass}, \
    curl::Bearer{loc.bearer}, \
    curl::PubKey{loc.pub_key}, \
    curl::PrivKey{loc.priv_key}, \
    curl::Port(loc.port)

auto Init() -> bool;
void Exit();
void ExitSignal();

// sync functions
auto ToMemory(const Api& e) -> ApiResult;
auto ToFile(const Api& e) -> ApiResult;
auto FromMemory(const Api& e) -> ApiResult;
auto FromFile(const Api& e) -> ApiResult;

// async functions
auto ToMemoryAsync(const Api& e) -> bool;
auto ToFileAsync(const Api& e) -> bool;
auto FromMemoryAsync(const Api& e) -> bool;
auto FromFileAsync(const Api& e) -> bool;

// uses curl to convert string to their %XX
auto EscapeString(const std::string& str) -> std::string;

struct Api {
    Api() = default;

    template <typename... Ts>
    Api(Ts&&... ts) {
        Api::set_option(std::forward<Ts>(ts)...);
    }

    template <typename... Ts>
    auto To(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return ToFile(std::forward<Ts>(ts)...);
        } else {
            return ToMemory(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto From(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return FromFile(std::forward<Ts>(ts)...);
        } else {
            return FromMemory(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto ToAsync(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return ToFileAsync(std::forward<Ts>(ts)...);
        } else {
            return ToMemoryAsync(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto FromAsync(Ts&&... ts) {
        if constexpr(std::disjunction_v<std::is_same<Path, Ts>...>) {
            return FromFileAsync(std::forward<Ts>(ts)...);
        } else {
            return FromMemoryAsync(std::forward<Ts>(ts)...);
        }
    }

    template <typename... Ts>
    auto ToMemory(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemory(*this);
    }

    template <typename... Ts>
    auto FromMemory(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<UploadInfo, Ts>...>, "UploadInfo must be specified");
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::FromMemory(*this);
    }

    template <typename... Ts>
    auto ToFile(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFile(*this);
    }

    template <typename... Ts>
    auto FromFile(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(std::disjunction_v<std::is_same<UploadInfo, Ts>...>, "UploadInfo must be specified");
        static_assert(!std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must not be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::FromFile(*this);
    }

    template <typename... Ts>
    auto ToMemoryAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToMemoryAsync(*this);
    }

    template <typename... Ts>
    auto FromMemoryAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<UploadInfo, Ts>...>, "UploadInfo must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(!std::disjunction_v<std::is_same<Path, Ts>...>, "Path must not valid for memory");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::FromMemoryAsync(*this);
    }

    template <typename... Ts>
    auto ToFileAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::ToFileAsync(*this);
    }

    template <typename... Ts>
    auto FromFileAsync(Ts&&... ts) {
        static_assert(std::disjunction_v<std::is_same<Url, Ts>...>, "Url must be specified");
        static_assert(std::disjunction_v<std::is_same<Path, Ts>...>, "Path must be specified");
        static_assert(std::disjunction_v<std::is_same<UploadInfo, Ts>...>, "UploadInfo must be specified");
        static_assert(std::disjunction_v<std::is_same<OnComplete, Ts>...>, "OnComplete must be specified");
        static_assert(std::disjunction_v<std::is_same<StopToken, Ts>...>, "StopToken must be specified");
        Api::set_option(std::forward<Ts>(ts)...);
        return curl::FromFileAsync(*this);
    }

    void SetUpload(bool enable) { m_is_upload = enable; }

    auto IsUpload() const { return m_is_upload; }
    auto& GetUrl() const { return m_url.m_str; }
    auto& GetFields() const { return m_fields.m_str; }
    auto& GetHeader() const { return m_header; }
    auto& GetFlags() const { return m_flags.m_flags; }
    auto& GetPath() const { return m_path; }
    auto& GetPort() const { return m_port.m_port; }
    auto& GetCustomRequest() const { return m_custom_request.m_str; }
    auto& GetUserPass() const { return m_userpass; }
    auto& GetPreemptiveAuth() const { return m_preemptive_auth.m_enable; }
    auto& GetBearer() const { return m_bearer.m_str; }
    auto& GetPubKey() const { return m_pub_key.m_str; }
    auto& GetPrivKey() const { return m_priv_key.m_str; }
    auto& GetUploadInfo() const { return m_info; }
    auto& GetOnComplete() const { return m_on_complete; }
    auto& GetOnProgress() const { return m_on_progress; }
    auto& GetOnUploadSeek() const { return m_on_upload_seek; }
    auto& GetPriority() const { return m_prio; }
    auto& GetToken() const { return m_stoken; }

    void SetOption(Url&& v) { m_url = v; }
    void SetOption(Fields&& v) { m_fields = v; }
    void SetOption(Header&& v) { m_header = v; }
    void SetOption(Flags&& v) { m_flags = v; }
    void SetOption(Path&& v) { m_path = v; }
    void SetOption(Port&& v) { m_port = v; }
    void SetOption(CustomRequest&& v) { m_custom_request = v; }
    void SetOption(UserPass&& v) { m_userpass = v; }
    void SetOption(PreemptiveAuth&& v) { m_preemptive_auth = v; }
    void SetOption(Bearer&& v) { m_bearer = v; }
    void SetOption(PubKey&& v) { m_pub_key = v; }
    void SetOption(PrivKey&& v) { m_priv_key = v; }
    void SetOption(UploadInfo&& v) { m_info = v; }
    void SetOption(OnComplete&& v) { m_on_complete = v; }
    void SetOption(OnProgress&& v) { m_on_progress = v; }
    void SetOption(OnUploadSeek&& v) { m_on_upload_seek = v; }
    void SetOption(Priority&& v) { m_prio = v; }
    void SetOption(StopToken&& v) { m_stoken = v; }

    template <typename T>
    void set_option(T&& t) {
        SetOption(std::forward<T>(t));
    }

    template <typename T, typename... Ts>
    void set_option(T&& t, Ts&&... ts) {
        set_option(std::forward<T>(t));
        set_option(std::forward<Ts>(ts)...);
    }

private:
    Url m_url{};
    Fields m_fields{};
    Header m_header{};
    Flags m_flags{};
    Path m_path{};
    Port m_port{};
    CustomRequest m_custom_request{};
    UserPass m_userpass{};
    PreemptiveAuth m_preemptive_auth{};
    Bearer m_bearer{};
    PubKey m_pub_key{};
    PrivKey m_priv_key{};
    UploadInfo m_info{};
    OnComplete m_on_complete{};
    OnProgress m_on_progress{};
    OnUploadSeek m_on_upload_seek{};
    Priority m_prio{Priority::High};
    std::stop_source m_stop_source{};
    StopToken m_stoken{m_stop_source.get_token()};
    bool m_is_upload{};
};

} // namespace sphaira::curl
