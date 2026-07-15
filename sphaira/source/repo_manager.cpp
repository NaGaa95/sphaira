#include "repo_manager.hpp"

#include "app.hpp"
#include "log.hpp"

#include <minIni.h>
#include <switch.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <strings.h>

namespace sphaira {
namespace {

constexpr const char* INI_SECTION = "appstore.repos";

auto BuildRepoKey(std::size_t index) -> std::string {
    return "repo_" + std::to_string(index);
}

auto ParseBoolFromField(std::string_view value, bool fallback = true) -> bool {
    if (value.empty()) {
        return fallback;
    }

    return value == "1" || value == "true" || value == "TRUE";
}

} // namespace

auto RepoManager::Get() -> RepoManager& {
    static RepoManager manager{};
    return manager;
}

RepoManager::RepoManager() {
    Load();
}

void RepoManager::Reload() {
    m_repos.clear();
    m_active_repo = 0;
    m_merge_mode = false;
    m_last_saved_repo_count = 0;
    Load();
}

void RepoManager::Load() {
    const auto count = static_cast<std::size_t>(std::max(0L, ini_getl(INI_SECTION, "repo_count", 0, App::CONFIG_PATH)));
    m_last_saved_repo_count = count;

    for (std::size_t i = 0; i < count; i++) {
        char buf[PATH_MAX]{};
        const auto key = BuildRepoKey(i);
        if (!ini_gets(INI_SECTION, key.c_str(), "", buf, sizeof(buf), App::CONFIG_PATH)) {
            continue;
        }

        std::string raw = buf;
        if (raw.empty()) {
            continue;
        }

        std::string fields[4];
        std::size_t part{};
        std::size_t begin{};

        while (part < 4) {
            const auto pos = raw.find('|', begin);
            if (pos == raw.npos) {
                fields[part++] = raw.substr(begin);
                break;
            }

            fields[part++] = raw.substr(begin, pos - begin);
            begin = pos + 1;
        }

        RepoConfig repo{};
        repo.url = Trim(fields[0]);
        repo.name = Trim(fields[1]);
        repo.enabled = ParseBoolFromField(fields[2], true);
        repo.priority = fields[3].empty() ? static_cast<long>(i) : std::strtol(fields[3].c_str(), nullptr, 10);

        if (repo.url.empty()) {
            continue;
        }

        if (repo.name.empty()) {
            repo.name = "Repo " + std::to_string(i + 1);
        }

        m_repos.emplace_back(repo);
    }

    m_active_repo = static_cast<std::size_t>(std::max(0L, ini_getl(INI_SECTION, "active_repo", 0, App::CONFIG_PATH)));
    m_merge_mode = ini_getbool(INI_SECTION, "merge_mode", false, App::CONFIG_PATH);

    EnsureDefaults();
    NormalizePriorities();
    if (m_active_repo >= m_repos.size()) {
        m_active_repo = 0;
    }
}

void RepoManager::EnsureDefaults() {
    if (m_repos.empty()) {
        m_repos.push_back({
            .url = DEFAULT_REPO_URL,
            .name = "Official Store",
            .enabled = true,
            .priority = 0,
        });
    }
}

void RepoManager::NormalizePriorities() {
    std::sort(m_repos.begin(), m_repos.end(), [](const auto& lhs, const auto& rhs){
        if (lhs.priority == rhs.priority) {
            return strcasecmp(lhs.url.c_str(), rhs.url.c_str()) < 0;
        }
        return lhs.priority < rhs.priority;
    });

    for (std::size_t i = 0; i < m_repos.size(); i++) {
        m_repos[i].priority = i;
    }
}

void RepoManager::Save() {
    NormalizePriorities();

    ini_putl(INI_SECTION, "repo_count", static_cast<long>(m_repos.size()), App::CONFIG_PATH);
    ini_putl(INI_SECTION, "active_repo", static_cast<long>(m_active_repo), App::CONFIG_PATH);
    ini_putl(INI_SECTION, "merge_mode", m_merge_mode ? 1L : 0L, App::CONFIG_PATH);

    for (std::size_t i = 0; i < m_repos.size(); i++) {
        const auto key = BuildRepoKey(i);
        const auto& repo = m_repos[i];
        const auto line = repo.url + "|" + repo.name + "|" + (repo.enabled ? "1" : "0") + "|" + std::to_string(repo.priority);
        ini_puts(INI_SECTION, key.c_str(), line.c_str(), App::CONFIG_PATH);
    }

    for (long i = static_cast<long>(m_repos.size()); i < m_last_saved_repo_count; i++) {
        const auto key = BuildRepoKey(i);
        ini_puts(INI_SECTION, key.c_str(), nullptr, App::CONFIG_PATH);
    }

    m_last_saved_repo_count = m_repos.size();
}

auto RepoManager::Trim(std::string value) const -> std::string {
    const auto is_space = [](unsigned char c){ return std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c){ return !is_space(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c){ return !is_space(c); }).base(), value.end());
    return value;
}

auto RepoManager::AddRepo(const std::string& url, const std::string& name) -> bool {
    auto trimmed_url = Trim(url);
    if (trimmed_url.empty()) {
        return false;
    }

    for (const auto& repo : m_repos) {
        if (!strcasecmp(repo.url.c_str(), trimmed_url.c_str())) {
            return false;
        }
    }

    m_repos.push_back({
        .url = trimmed_url,
        .name = name.empty() ? ("Repo " + std::to_string(m_repos.size() + 1)) : name,
        .enabled = true,
        .priority = static_cast<long>(m_repos.size()),
    });
    Save();
    return true;
}

auto RepoManager::RemoveRepo(std::size_t index) -> bool {
    if (index >= m_repos.size()) {
        return false;
    }

    m_repos.erase(m_repos.begin() + index);
    EnsureDefaults();

    if (m_active_repo >= m_repos.size()) {
        m_active_repo = m_repos.empty() ? 0 : m_repos.size() - 1;
    } else if (m_active_repo > index) {
        m_active_repo--;
    }

    Save();
    return true;
}

auto RepoManager::MoveRepoUp(std::size_t index) -> bool {
    if (!index || index >= m_repos.size()) {
        return false;
    }

    std::swap(m_repos[index], m_repos[index - 1]);

    if (m_active_repo == index) {
        m_active_repo = index - 1;
    } else if (m_active_repo == index - 1) {
        m_active_repo = index;
    }

    Save();
    return true;
}

auto RepoManager::MoveRepoDown(std::size_t index) -> bool {
    if (index + 1 >= m_repos.size()) {
        return false;
    }

    std::swap(m_repos[index], m_repos[index + 1]);

    if (m_active_repo == index) {
        m_active_repo = index + 1;
    } else if (m_active_repo == index + 1) {
        m_active_repo = index;
    }

    Save();
    return true;
}

auto RepoManager::SetRepoEnabled(std::size_t index, bool enabled) -> bool {
    if (index >= m_repos.size()) {
        return false;
    }

    m_repos[index].enabled = enabled;
    Save();
    return true;
}

auto RepoManager::SetActiveRepo(std::size_t index) -> bool {
    if (index >= m_repos.size()) {
        return false;
    }

    m_active_repo = index;
    Save();
    return true;
}

void RepoManager::SetMergeMode(bool enabled) {
    m_merge_mode = enabled;
    Save();
}

auto RepoManager::GetActiveRepoIndex() const -> std::size_t {
    if (m_active_repo >= m_repos.size()) {
        return 0;
    }
    return m_active_repo;
}

auto RepoManager::GetDownloadRepos() const -> std::vector<RepoConfig> {
    std::vector<RepoConfig> out;

    if (m_repos.empty()) {
        return out;
    }

    if (m_merge_mode) {
        for (const auto& repo : m_repos) {
            if (repo.enabled) {
                out.emplace_back(repo);
            }
        }
    } else {
        const auto index = GetActiveRepoIndex();
        if (index < m_repos.size() && m_repos[index].enabled) {
            out.emplace_back(m_repos[index]);
        } else {
            for (const auto& repo : m_repos) {
                if (repo.enabled) {
                    out.emplace_back(repo);
                    break;
                }
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs){
        return lhs.priority < rhs.priority;
    });

    return out;
}

auto RepoManager::GetRepoCachePath(std::string_view repo_url) const -> fs::FsPath {
    fs::FsPath out;
    const auto hash = crc32Calculate(repo_url.data(), repo_url.size());
    std::snprintf(out, sizeof(out), "/switch/sphaira/cache/appstore/repos/%08X", hash);
    return out;
}

auto RepoManager::GetRepoJsonCachePath(std::string_view repo_url) const -> fs::FsPath {
    return fs::AppendPath(GetRepoCachePath(repo_url), "repo.json");
}

} // namespace sphaira
