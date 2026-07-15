#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/list.hpp"

namespace sphaira::ui::menu::multi_repo_settings {

struct Menu final : MenuBase {
    Menu(u32 flags);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Repo Settings"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;
    void OnFocusGained() override;

private:
    void SetIndex(s64 index);
    void RefreshSubHeading();
    void TryMoveSelectedUp();
    void TryMoveSelectedDown();
    void OpenOptions();
    void ReloadFromManager();

private:
    s64 m_index{};
    std::unique_ptr<List> m_list{};
};

} // namespace sphaira::ui::menu::multi_repo_settings
