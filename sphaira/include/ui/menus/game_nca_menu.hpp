#pragma once

#include "ui/menus/menu_base.hpp"
#include "ui/menus/game_meta_menu.hpp"
#include "ui/list.hpp"
#include "yati/nx/nca.hpp"
#include "yati/nx/ncm.hpp"
#include <span>
#include <memory>

namespace sphaira::ui::menu::game::meta_nca {

struct NcaEntry {
    NcmContentId content_id{};
    u64 size{};
    u8 content_type{};
    u8 attr{};
    // decrypted nca header.
    nca::Header header{};
    // set if missing.
    bool missing{};
    // set if selected.
    bool selected{};
    // set if we have checked the above meta data.
    bool checked{};
};

struct Menu final : MenuBase {
    Menu(Entry& entry, const meta::MetaEntry& meta_entry);
    ~Menu();

    auto GetShortTitle() const -> const char* override { return "Nca"; };
    void Update(Controller* controller, TouchInfo* touch) override;
    void Draw(NVGcontext* vg, Theme* theme) override;

private:
    void SetIndex(s64 index);
    void Scan();
    void UpdateSubheading();

    auto GetSelectedEntries() const {
        std::vector<NcaEntry> out;
        for (auto& e : m_entries) {
            if (e.selected && !e.missing) {
                out.emplace_back(e);
            }
        }

        if (!m_entries.empty() && out.empty()) {
            out.emplace_back(m_entries[m_index]);
        }

        return out;
    }

    void ClearSelection() {
        for (auto& e : m_entries) {
            e.selected = false;
        }

        m_selected_count = 0;
    }

    auto GetEntry(u32 index) -> NcaEntry& {
        return m_entries[index];
    }

    auto GetEntry(u32 index) const -> const NcaEntry& {
        return m_entries[index];
    }

    auto GetEntry() -> NcaEntry& {
        return GetEntry(m_index);
    }

    auto GetEntry() const -> const NcaEntry& {
        return GetEntry(m_index);
    }

    void DumpNcas();
    Result MountNcaFs();

private:
    Entry& m_entry;
    const meta::MetaEntry& m_meta_entry;
    NcmMetaData m_meta{};
    std::vector<NcaEntry> m_entries{};
    s64 m_index{};
    s64 m_selected_count{};
    std::unique_ptr<List> m_list{};
};

} // namespace sphaira::ui::menu::game::meta_nca
