#include "ui/forwarder_editor.hpp"

#include "app.hpp"
#include "fs.hpp"
#include "i18n.hpp"
#include "nro.hpp"
#include "swkbd.hpp"
#include "ui/list.hpp"
#include "ui/menus/file_picker.hpp"
#include "ui/nvg_util.hpp"
#include "ui/option_box.hpp"
#include "ui/scrolling_text.hpp"
#include "ui/steamgriddb_icon.hpp"
#include "ui/widget.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace sphaira::ui::forwarder {
namespace {

enum class Row {
    Author,
    Version,
    ProfileSelection,
    AddressSpace,
    CpuCores,
    Screenshot,
    VideoCapture,
    NativeGame,
    SvcDebug,
    Create,
};

class Editor final : public Widget {
public:
    explicit Editor(Config config)
    : m_values{std::move(config.values)}
    , m_icon_source{config.icon_source.empty() ? "Default"_i18n : std::move(config.icon_source)}
    , m_steam_query{std::move(config.steam_query)}
    , m_screen_title{config.screen_title.empty() ? "Forwarder Creation"_i18n : std::move(config.screen_title)}
    , m_title_label{config.title_label.empty() ? "App Title"_i18n : std::move(config.title_label)}
    , m_submit_label{config.submit_label.empty() ? "Create Forwarder"_i18n : std::move(config.submit_label)}
    , m_show_author{config.show_author}
    , m_show_version{config.show_version}
    , m_show_forwarder_options{config.show_forwarder_options}
    , m_on_create{std::move(config.on_create)} {
        SetActions(
            std::make_pair(Button::A, Action{"Select"_i18n, [this](){ Activate(); }}),
            std::make_pair(Button::B, Action{"Back"_i18n, [this](){ SetPop(); }})
        );

        if (m_show_author) {
            m_rows.emplace_back(Row::Author);
        }
        if (m_show_version) {
            m_rows.emplace_back(Row::Version);
        }
        if (m_show_forwarder_options) {
            m_rows.emplace_back(Row::ProfileSelection);
            m_rows.emplace_back(Row::AddressSpace);
            m_rows.emplace_back(Row::CpuCores);
            m_rows.emplace_back(Row::Screenshot);
            m_rows.emplace_back(Row::VideoCapture);
            m_rows.emplace_back(Row::NativeGame);
            m_rows.emplace_back(Row::SvcDebug);
        }
        m_rows.emplace_back(Row::Create);

        const Vec4 list_pos{445.f, 105.f, 780.f, 525.f};
        const Vec4 item_pos{465.f, 115.f, 735.f, 62.f};
        m_list = std::make_unique<List>(1, 7, list_pos, item_pos, Vec2{0.f, 7.f});
        m_list->SetPageJump(false);
        m_list->SetScrollBarPos(1225.f, 115.f, 505.f);

        auto use_default_icon = m_values.icon.empty();
        auto normalized = steamgriddb::NormalizeIcon(
            use_default_icon ? App::GetDefaultImageData() : std::span<const u8>{m_values.icon}
        );
        if (normalized.empty()) {
            use_default_icon = true;
            normalized = steamgriddb::NormalizeIcon(App::GetDefaultImageData());
        }
        if (normalized.empty()) {
            normalized.assign(App::GetDefaultImageData().begin(), App::GetDefaultImageData().end());
        }

        m_values.icon = std::move(normalized);
        if (use_default_icon) {
            m_icon_source = "Default"_i18n;
        }
        UpdatePreview();
    }

    ~Editor() override {
        if (m_preview > 0) {
            nvgDeleteImage(App::GetVg(), m_preview);
        }
    }

    void Update(Controller* controller, TouchInfo* touch) override {
        Widget::Update(controller, touch);

        if (touch->is_clicked && touch->in_range(m_title_box)) {
            if (!m_title_focused) {
                App::PlaySoundEffect(SoundEffect::Focus);
            }
            m_title_focused = true;
            m_icon_focused = false;
            EditText(m_values.title, m_title_label, 1, sizeof(NacpLanguageEntry::name) - 1);
            return;
        }

        if (touch->is_clicked && touch->in_range(m_icon_box)) {
            if (!m_icon_focused) {
                App::PlaySoundEffect(SoundEffect::Focus);
            }
            m_icon_focused = true;
            m_title_focused = false;
            ChooseIconSource();
            return;
        }

        if (m_title_focused) {
            if (controller->GotDown(Button::DOWN)) {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_title_focused = false;
                m_icon_focused = true;
                return;
            } else if (controller->GotDown(Button::RIGHT)) {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_title_focused = false;
                return;
            }
        }

        if (m_icon_focused) {
            if (controller->GotDown(Button::UP)) {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_icon_focused = false;
                m_title_focused = true;
                return;
            } else if (controller->GotDown(Button::RIGHT)) {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_icon_focused = false;
                return;
            }
        }

        if (controller->GotDown(Button::LEFT)) {
            App::PlaySoundEffect(SoundEffect::Focus);
            m_icon_focused = true;
            m_title_focused = false;
            return;
        }

        if ((m_title_focused || m_icon_focused)
            && !touch->is_clicked && !touch->is_scroll && !touch->is_end) {
            return;
        }

        m_list->OnUpdate(controller, touch, m_index, m_rows.size(), [this](bool touched, s64 index){
            if (touched && m_index == index) {
                m_icon_focused = false;
                m_title_focused = false;
                FireAction(Button::A);
            } else {
                App::PlaySoundEffect(SoundEffect::Focus);
                m_icon_focused = false;
                m_title_focused = false;
                m_index = index;
            }
        });
    }

    void Draw(NVGcontext* vg, Theme* theme) override {
        DrawElement(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, ThemeEntryID_BACKGROUND);
        m_scroll_screen_title.Draw(vg, true, 70.f, 55.f, 520.f, 30.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT), m_screen_title);

        gfx::drawRect(vg, 30.f, 86.f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));
        gfx::drawRect(vg, 30.f, 646.f, 1220.f, 1.f, theme->GetColour(ThemeEntryID_LINE));

        const Vec4 icon_panel{55.f, 110.f, 350.f, 510.f};
        DrawElement(icon_panel, ThemeEntryID_GRID);

        DrawElement(m_title_box, ThemeEntryID_GRID);
        if (m_title_focused) {
            gfx::drawRectOutline(vg, theme, 4.f, m_title_box);
        }
        gfx::drawText(vg, 90.f, 140.f, 16.f, theme->GetColour(ThemeEntryID_TEXT_INFO), m_title_label.c_str(), NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        m_scroll_app_title.Draw(vg, m_title_focused, 90.f, 168.f, 280.f, 20.f,
            NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(m_title_focused ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT), m_values.title);

        const auto preview = m_preview > 0 ? m_preview : App::GetDefaultImage();
        if (m_icon_focused) {
            gfx::drawRectOutline(vg, theme, 4.f, m_icon_box);
        }
        gfx::drawImage(vg, m_icon_box, preview, 8.f);
        gfx::drawText(vg, 230.f, 520.f, 22.f, theme->GetColour(m_icon_focused ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT), "Icon"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        gfx::drawText(vg, 230.f, 550.f, 17.f, theme->GetColour(ThemeEntryID_TEXT_INFO), m_icon_source.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        gfx::drawTextBox(vg, 85.f, 580.f, 16.f, 290.f, theme->GetColour(ThemeEntryID_TEXT_INFO), "Press A or tap the icon to change it"_i18n.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_TOP);

        m_list->Draw(vg, theme, m_rows.size(), [this](auto* vg, auto* theme, const Vec4& pos, s64 index){
            const auto row = m_rows[index];
            const auto selected = !m_title_focused && !m_icon_focused && m_index == index;
            DrawElement(pos, ThemeEntryID_GRID);
            if (selected) {
                gfx::drawRectOutline(vg, theme, 4.f, pos);
                gfx::drawRect(vg, pos.x + 4.f, pos.y + 10.f, 3.f, pos.h - 20.f, theme->GetColour(ThemeEntryID_TEXT_SELECTED));
            }

            const auto colour = theme->GetColour(selected ? ThemeEntryID_TEXT_SELECTED : ThemeEntryID_TEXT);
            if (row == Row::Create) {
                gfx::drawText(vg, pos.x + pos.w / 2.f, pos.y + pos.h / 2.f, 23.f, colour, m_submit_label.c_str(), NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                return;
            }

            const auto label = GetRowLabel(row);
            const auto value = GetRowValue(row);
            constexpr float row_padding = 20.f;
            constexpr float column_gap = 20.f;
            const auto label_x = pos.x + row_padding;
            const auto value_right = pos.x + pos.w - row_padding;
            const auto content_width = std::max(0.f, pos.w - row_padding * 2.f);

            float value_bounds[4]{};
            nvgFontSize(vg, 20.f);
            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgTextBounds(vg, 0.f, 0.f, value.c_str(), nullptr, value_bounds);
            const auto measured_value_width = std::max(0.f, value_bounds[2] - value_bounds[0]);
            const auto value_width = std::clamp(measured_value_width, 72.f, content_width * 0.45f);
            const auto value_x = value_right - value_width;
            const auto label_width = std::max(0.f, value_x - column_gap - label_x);
            const auto value_draw_x = measured_value_width < value_width
                ? value_right - measured_value_width
                : value_x;

            m_scroll_row_label.Draw(vg, selected, label_x, pos.y + pos.h / 2.f, label_width, 19.f,
                NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_INFO), label);
            m_scroll_row_value.Draw(vg, selected, value_draw_x, pos.y + pos.h / 2.f,
                std::max(0.f, value_right - value_draw_x), 20.f, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE,
                colour, value);
        });

        Widget::Draw(vg, theme);
    }

private:
    void Activate() {
        if (m_title_focused) {
            EditText(m_values.title, m_title_label, 1, sizeof(NacpLanguageEntry::name) - 1);
            return;
        }

        if (m_icon_focused) {
            ChooseIconSource();
            return;
        }

        switch (m_rows[m_index]) {
            case Row::Author:
                EditText(m_values.author, "Author"_i18n, 1, sizeof(NacpLanguageEntry::author) - 1);
                break;
            case Row::Version:
                EditText(m_values.version, "Version"_i18n, 1, sizeof(NacpStruct::display_version) - 1);
                break;
            case Row::ProfileSelection:
                m_values.profile_selection = !m_values.profile_selection;
                break;
            case Row::AddressSpace:
                switch (m_values.address_space) {
                    case ForwarderAddressSpace::Bit36: m_values.address_space = ForwarderAddressSpace::Bit39; break;
                    case ForwarderAddressSpace::Bit39: m_values.address_space = ForwarderAddressSpace::Bit32; break;
                    case ForwarderAddressSpace::Bit32: m_values.address_space = ForwarderAddressSpace::Bit32NoAlias; break;
                    case ForwarderAddressSpace::Bit32NoAlias: m_values.address_space = ForwarderAddressSpace::Bit36; break;
                }
                break;
            case Row::CpuCores:
                if (m_values.core_mode == CpuCoreMode::Four) {
                    m_values.core_mode = CpuCoreMode::Three;
                } else {
                    App::Push<OptionBox>(
                        "4 CPU Cores\n\nCore 3 is shared with system services. Only enable this for homebrew that manages thread affinity correctly; unrestricted use can cause system lag or instability."_i18n,
                        "Back"_i18n,
                        "Enable"_i18n,
                        0,
                        [this](auto index) {
                            if (index && *index == 1) {
                                m_values.core_mode = CpuCoreMode::Four;
                            }
                        }
                    );
                }
                break;
            case Row::Screenshot:
                m_values.screenshot = !m_values.screenshot;
                break;
            case Row::VideoCapture:
                m_values.video_capture = !m_values.video_capture;
                break;
            case Row::NativeGame:
                switch (m_values.native_game_mode) {
                    case ForwarderNativeGameMode::Automatic: m_values.native_game_mode = ForwarderNativeGameMode::Enabled; break;
                    case ForwarderNativeGameMode::Enabled: m_values.native_game_mode = ForwarderNativeGameMode::Disabled; break;
                    case ForwarderNativeGameMode::Disabled: m_values.native_game_mode = ForwarderNativeGameMode::Automatic; break;
                }
                break;
            case Row::SvcDebug:
                switch (m_values.svc_debug_mode) {
                    case ForwarderSvcDebugMode::Automatic: m_values.svc_debug_mode = ForwarderSvcDebugMode::Enabled; break;
                    case ForwarderSvcDebugMode::Enabled: m_values.svc_debug_mode = ForwarderSvcDebugMode::Disabled; break;
                    case ForwarderSvcDebugMode::Disabled: m_values.svc_debug_mode = ForwarderSvcDebugMode::Automatic; break;
                }
                break;
            case Row::Create:
                Create();
                break;
        }
    }

    void EditText(std::string& value, const std::string& header, s64 min_length, s64 max_length) {
        std::string output;
        if (R_SUCCEEDED(swkbd::ShowText(output, header.c_str(), header.c_str(), value.c_str(), min_length, max_length)) && !output.empty()) {
            value = std::move(output);
        }
    }

    void ChooseIconSource() {
        App::Push<OptionBox>(
            "Choose Icon Source"_i18n,
            "Local File"_i18n,
            "SteamGridDB"_i18n,
            [this](auto index){
                if (!index) {
                    return;
                }
                if (*index == 0) {
                    SelectLocalIcon();
                } else {
                    SearchSteamGridDb();
                }
            },
            m_preview
        );
    }

    void SelectLocalIcon() {
        const std::vector<std::string> filters{"nro", "png", "jpg", "jpeg"};
        App::Push<menu::filebrowser::picker::Menu>(
            [this](const fs::FsPath& path){
                std::vector<u8> icon;
                const auto string_path = path.toString();
                const auto extension = string_path.find_last_of('.');
                const auto is_nro = extension != std::string::npos && !strcasecmp(string_path.c_str() + extension + 1, "nro");
                if (is_nro) {
                    icon = nro_get_icon(path);
                } else if (R_FAILED(fs::FsStdio().read_entire_file(path, icon))) {
                    App::Notify("Failed to load icon"_i18n);
                    return false;
                }

                auto normalized = steamgriddb::NormalizeIcon(icon);
                if (normalized.empty()) {
                    App::Notify("The selected icon is not a valid image"_i18n);
                    return false;
                }

                auto source = string_path;
                if (const auto slash = source.find_last_of('/'); slash != std::string::npos) {
                    source.erase(0, slash + 1);
                }
                SetIcon(std::move(normalized), std::move(source));
                return true;
            },
            filters
        );
    }

    void SearchSteamGridDb() {
        const auto query = m_steam_query.empty() ? m_values.title : m_steam_query;
        steamgriddb::ShowIconPicker(query, [this](auto icon){
            SetIcon(std::move(icon), "SteamGridDB"_i18n);
        });
    }

    void SetIcon(std::vector<u8> icon, std::string source) {
        m_values.icon = std::move(icon);
        m_icon_source = std::move(source);
        UpdatePreview();
    }

    void UpdatePreview() {
        if (m_preview > 0) {
            nvgDeleteImage(App::GetVg(), m_preview);
            m_preview = 0;
        }
        if (!m_values.icon.empty()) {
            m_preview = nvgCreateImageMem(App::GetVg(), 0, m_values.icon.data(), m_values.icon.size());
        }
    }

    void Create() {
        if (m_values.title.empty() || m_values.icon.empty()
            || (m_show_author && m_values.author.empty())
            || (m_show_version && m_values.version.empty())) {
            App::Notify("The required fields and icon must be non-empty"_i18n);
            return;
        }

        if (m_on_create && m_on_create(m_values)) {
            SetPop();
        }
    }

    auto GetRowLabel(Row row) const -> std::string {
        switch (row) {
            case Row::Author: return "Author"_i18n;
            case Row::Version: return "Version"_i18n;
            case Row::ProfileSelection: return "Profile Selection"_i18n;
            case Row::AddressSpace: return "Address Space"_i18n;
            case Row::CpuCores: return "CPU Cores"_i18n;
            case Row::Screenshot: return "Screenshots"_i18n;
            case Row::VideoCapture: return "Video Capture"_i18n;
            case Row::NativeGame: return "Native Game (FPSLocker)"_i18n;
            case Row::SvcDebug: return "svcDebug Mode"_i18n;
            case Row::Create: return {};
        }
        return {};
    }

    auto GetRowValue(Row row) const -> std::string {
        switch (row) {
            case Row::Author: return m_values.author;
            case Row::Version: return m_values.version;
            case Row::ProfileSelection: return m_values.profile_selection ? "Enabled"_i18n : "Disabled"_i18n;
            case Row::AddressSpace:
                switch (m_values.address_space) {
                    case ForwarderAddressSpace::Bit36: return "36-bit (Default)"_i18n;
                    case ForwarderAddressSpace::Bit39: return "39-bit"_i18n;
                    case ForwarderAddressSpace::Bit32: return "32-bit"_i18n;
                    case ForwarderAddressSpace::Bit32NoAlias: return "32-bit (no alias)"_i18n;
                }
                return {};
            case Row::CpuCores: return m_values.core_mode == CpuCoreMode::Three ? "3 (Default)"_i18n : "4 (Advanced)"_i18n;
            case Row::Screenshot: return m_values.screenshot ? "Enabled"_i18n : "Disabled"_i18n;
            case Row::VideoCapture: return m_values.video_capture ? "Enabled"_i18n : "Disabled"_i18n;
            case Row::NativeGame:
                switch (m_values.native_game_mode) {
                    case ForwarderNativeGameMode::Automatic: return "Automatic (Default)"_i18n;
                    case ForwarderNativeGameMode::Enabled: return "Enabled"_i18n;
                    case ForwarderNativeGameMode::Disabled: return "Disabled"_i18n;
                }
                break;
            case Row::SvcDebug:
                switch (m_values.svc_debug_mode) {
                    case ForwarderSvcDebugMode::Automatic: return "Automatic (Default)"_i18n;
                    case ForwarderSvcDebugMode::Enabled: return "Enabled"_i18n;
                    case ForwarderSvcDebugMode::Disabled: return "Disabled"_i18n;
                }
                break;
            case Row::Create: return {};
        }
        return {};
    }

private:
    Values m_values;
    std::string m_icon_source;
    const std::string m_steam_query;
    const std::string m_screen_title;
    const std::string m_title_label;
    const std::string m_submit_label;
    const bool m_show_author;
    const bool m_show_version;
    const bool m_show_forwarder_options;
    const CreateCallback m_on_create;
    std::vector<Row> m_rows;
    std::unique_ptr<List> m_list;
    const Vec4 m_title_box{75.f, 120.f, 310.f, 70.f};
    const Vec4 m_icon_box{80.f, 205.f, 300.f, 300.f};
    int m_preview{};
    s64 m_index{};
    bool m_icon_focused{true};
    bool m_title_focused{};
    ScrollingText m_scroll_screen_title{};
    ScrollingText m_scroll_app_title{};
    ScrollingText m_scroll_row_label{};
    ScrollingText m_scroll_row_value{};
};

} // namespace

void Show(Config config) {
    App::Push<Editor>(std::move(config));
}

} // namespace sphaira::ui::forwarder
