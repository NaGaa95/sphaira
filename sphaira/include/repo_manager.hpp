#pragma once

#include "fs.hpp"
#include <string>
#include <vector>

namespace sphaira {

struct RepoConfig {
    std::string url{};
    std::string name{};
    bool enabled{true};
    long priority{};
};

class RepoManager {
public:
    static constexpr inline const char* DEFAULT_REPO_URL = "https://switch.cdn.fortheusers.org/repo.json";

    static auto Get() -> RepoManager&;

    auto GetRepos() -> std::vector<RepoConfig>& { return m_repos; }
    auto GetRepos() const -> const std::vector<RepoConfig>& { return m_repos; }
    auto GetActiveRepoIndex() const -> std::size_t;
    auto IsMergeMode() const -> bool { return m_merge_mode; }

    void Reload();
    void Save();

    auto AddRepo(const std::string& url, const std::string& name) -> bool;
    auto RemoveRepo(std::size_t index) -> bool;
    auto MoveRepoUp(std::size_t index) -> bool;
    auto MoveRepoDown(std::size_t index) -> bool;
    auto SetRepoEnabled(std::size_t index, bool enabled) -> bool;
    auto SetActiveRepo(std::size_t index) -> bool;
    void SetMergeMode(bool enabled);

    auto GetDownloadRepos() const -> std::vector<RepoConfig>;
    auto GetRepoCachePath(std::string_view repo_url) const -> fs::FsPath;
    auto GetRepoJsonCachePath(std::string_view repo_url) const -> fs::FsPath;

private:
    RepoManager();

    void Load();
    void EnsureDefaults();
    void NormalizePriorities();
    auto Trim(std::string value) const -> std::string;

private:
    std::vector<RepoConfig> m_repos{};
    std::size_t m_active_repo{};
    bool m_merge_mode{};
    long m_last_saved_repo_count{};
};

} // namespace sphaira
