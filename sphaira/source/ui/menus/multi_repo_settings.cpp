#include "ui/menus/multi_repo_settings.hpp"

#include "app.hpp"
#include "i18n.hpp"
#include "repo_manager.hpp"
#include "swkbd.hpp"
#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/sidebar.hpp"

namespace sphaira::ui::menu::multi_repo_settings {

Menu::Menu(u32 flags) : MenuBase{"AppStore Repos"_i18n, flags} {
    const Vec4 v{75, GetY() + 1.f + 42.f, 1220.f - 45.f * 2, 60};
    m_list = std::make_unique<List>(1, 8, m_pos, v);

    SetActions(
        std::make_pair(Button::A, Action{"Set Active"_i18n, [this](){
            auto& manager = RepoManager::Get();
            auto& repos = manager.GetRepos();
            if (repos.empty() || m_index < 0 || m_index >= static_cast<s64>(repos.size())) {
                return;
            }

            manager.SetActiveRepo(static_cast<std::size_t>(m_index));
            RefreshSubHeading();
        }}),
        std::make_pair(Button::B, Action{"Back"_i18n, [this](){ SetPop(); }}),
        std::make_pair(Button::X, Action{"Options"_i18n, [this](){
            OpenOptions();
        }}),
        std::make_pair(Button::R | Button::UP, Action{"Move Up"_i18n, [this](){
            TryMoveSelectedUp();
        }}),
        std::make_pair(Button::R | Button::DOWN, Action{"Move Down"_i18n, [this](){
            TryMoveSelectedDown();
        }})
    );
}

Menu::~Menu() = default;

void Menu::OnFocusGained() {
    MenuBase::OnFocusGained();
    ReloadFromManager();
}

void Menu::Update(Controller* controller, TouchInfo* touch) {
    MenuBase::Update(controller, touch);

    if (controller->Got(Button::R)) {
        if (controller->GotDown(Button::UP)) {
            TryMoveSelectedUp();
            return;
        } else if (controller->GotDown(Button::DOWN)) {
            TryMoveSelectedDown();
            return;
        }
    }

    const auto count = RepoManager::Get().GetRepos().size();
    m_list->OnUpdate(controller, touch, m_index, count, [this](bool touch, auto i) {
        if (touch && m_index == i) {
            FireAction(Button::A);
        } else {
            App::PlaySoundEffect(SoundEffect::Focus);
            SetIndex(i);
        }
    });
}

void Menu::Draw(NVGcontext* vg, Theme* theme) {
    MenuBase::Draw(vg, theme);

    auto& manager = RepoManager::Get();
    const auto& repos = manager.GetRepos();
    if (repos.empty()) {
        gfx::drawTextArgs(vg, SCREEN_WIDTH / 2.f, SCREEN_HEIGHT / 2.f, 36.f, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), "Empty..."_i18n.c_str());
        return;
    }

    constexpr float text_xoffset{15.f};
    constexpr float tag_gap{8.f};

    m_list->Draw(vg, theme, repos.size(), [this, &manager, &repos](auto* vg, auto* theme, auto& v, auto i) {
        const auto& [x, y, w, h] = v;
        const auto& repo = repos[i];

        auto text_id = ThemeEntryID_TEXT;
        if (m_index == static_cast<s64>(i)) {
            text_id = ThemeEntryID_TEXT_SELECTED;
            gfx::drawRectOutline(vg, theme, 4.f, v);
        } else if (i != repos.size() - 1) {
            gfx::drawRect(vg, x, y + h, w, 1.f, theme->GetColour(ThemeEntryID_LINE_SEPARATOR));
        }

        std::string left = std::to_string(i + 1) + ". " + repo.name;
        if (!repo.enabled) {
            left += " [OFF]";
        }
        if (manager.GetActiveRepoIndex() == i) {
            left += " [ACTIVE]";
        }

        nvgSave(vg);
        nvgIntersectScissor(vg, x + text_xoffset, y, w - 240.f, h);
            gfx::drawText(vg, x + text_xoffset, y + (h / 2.f), 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(text_id), left.c_str());
        nvgRestore(vg);

        gfx::drawText(vg, x + text_xoffset, y + h - tag_gap, 14.f, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, theme->GetColour(ThemeEntryID_TEXT_INFO), repo.url.c_str());
    });
}

void Menu::SetIndex(s64 index) {
    const auto count = static_cast<s64>(RepoManager::Get().GetRepos().size());
    if (!count) {
        m_index = 0;
        SetSubHeading("0 / 0");
        return;
    }

    if (index < 0) {
        index = 0;
    } else if (index >= count) {
        index = count - 1;
    }

    m_index = index;
    if (!m_index) {
        m_list->SetYoff(0);
    }

    RefreshSubHeading();
}

void Menu::RefreshSubHeading() {
    auto& manager = RepoManager::Get();
    const auto& repos = manager.GetRepos();
    const auto count = repos.size();
    const auto index = count ? static_cast<std::size_t>(m_index + 1) : 0;
    const auto mode = manager.IsMergeMode() ? "Merge all active repos"_i18n : "Only active repo"_i18n;
    SetSubHeading(std::to_string(index) + " / " + std::to_string(count) + " | " + mode);
}

void Menu::TryMoveSelectedUp() {
    auto& manager = RepoManager::Get();
    if (!manager.MoveRepoUp(static_cast<std::size_t>(m_index))) {
        return;
    }

    if (m_index > 0) {
        m_index--;
    }
    RefreshSubHeading();
}

void Menu::TryMoveSelectedDown() {
    auto& manager = RepoManager::Get();
    if (!manager.MoveRepoDown(static_cast<std::size_t>(m_index))) {
        return;
    }

    if (m_index + 1 < static_cast<s64>(manager.GetRepos().size())) {
        m_index++;
    }
    RefreshSubHeading();
}

void Menu::OpenOptions() {
    auto options = std::make_unique<Sidebar>("Repo Settings"_i18n, Sidebar::Side::RIGHT);
    ON_SCOPE_EXIT(App::Push(std::move(options)));

    auto& manager = RepoManager::Get();
    auto& repos = manager.GetRepos();

    options->Add<SidebarEntryBool>("Merge mode"_i18n, manager.IsMergeMode(), [this](bool& enabled){
        RepoManager::Get().SetMergeMode(enabled);
        RefreshSubHeading();
    }, "When enabled, all active repos are downloaded and merged."_i18n);

    if (repos.empty() || m_index < 0 || m_index >= static_cast<s64>(repos.size())) {
        return;
    }

    options->Add<SidebarEntryBool>("Enabled"_i18n, repos[m_index].enabled, [this](bool& enabled){
        RepoManager::Get().SetRepoEnabled(static_cast<std::size_t>(m_index), enabled);
    }, "Enable or disable the selected repo."_i18n);

    options->Add<SidebarEntryCallback>("Add repo"_i18n, [this](){
        std::string url;
        if (R_FAILED(swkbd::ShowText(url, "Repo URL"_i18n, "https://")) || url.empty()) {
            return;
        }

        std::string name;
        swkbd::ShowText(name, "Repo name"_i18n);
        if (name.empty()) {
            name = url;
        }

        if (!RepoManager::Get().AddRepo(url, name)) {
            App::Notify("Failed to add repo"_i18n);
        }

        ReloadFromManager();
    });

    options->Add<SidebarEntryCallback>("Remove repo"_i18n, [this](){
        auto& manager = RepoManager::Get();
        const auto repos = manager.GetRepos();
        if (repos.empty()) {
            return;
        }

        const auto selected = static_cast<std::size_t>(m_index);
        const auto name = repos[selected].name;
        App::Push<OptionBox>(
            i18n::Reorder("Remove repo: ", name) + "?",
            "Back"_i18n, "Remove"_i18n, 1,
            [this, selected](auto index) {
                if (!index || !*index) {
                    return;
                }

                RepoManager::Get().RemoveRepo(selected);
                ReloadFromManager();
            }
        );
    });
}

void Menu::ReloadFromManager() {
    auto& repos = RepoManager::Get().GetRepos();
    if (repos.empty()) {
        m_index = 0;
        RefreshSubHeading();
        return;
    }

    if (m_index >= static_cast<s64>(repos.size())) {
        m_index = repos.size() - 1;
    } else if (m_index < 0) {
        m_index = 0;
    }
    SetIndex(m_index);
}

} // namespace sphaira::ui::menu::multi_repo_settings
