#include "cores3_v4_display.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "device_state.h"
#include "lvgl_theme.h"
#include "nexus_wifi_qr.h"
#include "settings.h"
#include "ssid_manager.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_log.h>
#include <material_symbols.h>

#define TAG "CoreS3V4"

CoreS3V4Display::CoreS3V4Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                 int width, int height, int offset_x, int offset_y, bool mirror_x,
                                 bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy, /*clear_color=*/0x0000) {}

CoreS3V4Display::~CoreS3V4Display() {
    if (ready_timer_ != nullptr) {
        esp_timer_stop(ready_timer_);
        esp_timer_delete(ready_timer_);
        ready_timer_ = nullptr;
    }
}

const lv_font_t* CoreS3V4Display::TextFont() const {
    auto theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->text_font() != nullptr) {
        return theme->text_font()->font();
    }
    return LV_FONT_DEFAULT;
}

const lv_font_t* CoreS3V4Display::IconFont() const {
    auto theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->icon_font() != nullptr) {
        return theme->icon_font()->font();
    }
    return TextFont();
}

const lv_font_t* CoreS3V4Display::LargeIconFont() const {
    auto theme = static_cast<LvglTheme*>(current_theme_);
    if (theme != nullptr && theme->large_icon_font() != nullptr &&
        theme->large_icon_font()->font() != nullptr) {
        return theme->large_icon_font()->font();
    }
    return IconFont();
}

lv_obj_t* CoreS3V4Display::MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    if (font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    return label;
}

void CoreS3V4Display::PlaceCenter(lv_obj_t* obj, int cx, int cy) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_align(obj, LV_ALIGN_CENTER, cx - (LV_HOR_RES / 2), cy - (LV_VER_RES / 2));
}

void CoreS3V4Display::Hide(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void CoreS3V4Display::Show(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void CoreS3V4Display::HideAllContent() {
    Hide(title_label_);
    Hide(subtitle_label_);
    Hide(body_label_);
    Hide(icon_label_);
    Hide(qr_image_);
    Hide(status_time_);
    Hide(status_wifi_);
    Hide(status_battery_);
    Hide(nexus_logo_);
    Hide(listening_label_);
    Hide(icon_talk_);
    Hide(icon_task_);
    Hide(icon_volume_);
    Hide(icon_wifi_);
    Hide(ready_check_);
    Hide(emoji_box_);
}

void CoreS3V4Display::ShowV4Layer() {
    Hide(container_);
    Hide(top_bar_);
    Hide(status_bar_);
    Hide(bottom_bar_);
    if (v4_layer_ != nullptr) {
        Show(v4_layer_);
        lv_obj_set_style_bg_color(v4_layer_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(v4_layer_, LV_OPA_COVER, 0);
        lv_obj_move_foreground(v4_layer_);
    }
}

void CoreS3V4Display::ApplyMenuSelectionVisuals() {
    auto style = [](lv_obj_t* icon, bool selected) {
        if (icon == nullptr) {
            return;
        }
        lv_obj_set_style_text_color(icon, lv_color_hex(kLime), 0);
        lv_obj_set_style_text_opa(icon, selected ? LV_OPA_COVER : LV_OPA_40, 0);
    };
    style(icon_talk_, menu_selected_ == MenuItem::Talk);
    style(icon_task_, menu_selected_ == MenuItem::Tasks);
    style(icon_volume_, menu_selected_ == MenuItem::Volume);
    style(icon_wifi_, menu_selected_ == MenuItem::ChangeWifi);
    Show(icon_talk_);
    Show(icon_task_);
    Show(icon_volume_);
    Show(icon_wifi_);
}

void CoreS3V4Display::RefreshStatusRow() {
    auto& board = Board::GetInstance();
    if (status_time_ != nullptr) {
        time_t now = time(nullptr);
        struct tm* tm_now = localtime(&now);
        if (tm_now != nullptr && tm_now->tm_year >= (2025 - 1900)) {
            char buf[8];
            strftime(buf, sizeof(buf), "%H:%M", tm_now);
            lv_label_set_text(status_time_, buf);
        } else {
            lv_label_set_text(status_time_, "--:--");
        }
    }
    if (status_wifi_ != nullptr) {
        const char* icon = board.GetNetworkStateIcon();
        lv_label_set_text(status_wifi_, icon != nullptr ? icon : MATERIAL_SYMBOLS_WIFI_OFF);
    }
    if (status_battery_ != nullptr) {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        const char* icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL;
        if (board.GetBatteryLevel(level, charging, discharging)) {
            if (charging) {
                icon = MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_BOLT;
            } else {
                const char* levels[] = {
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
                    MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
                };
                int idx = level <= 0 ? 0 : (level >= 100 ? 7 : 1 + ((level - 1) * 6 / 99));
                icon = levels[idx];
            }
        }
        lv_label_set_text(status_battery_, icon);
    }
}

void CoreS3V4Display::ShowStatusRow(bool visible) {
    if (!visible) {
        Hide(status_time_);
        Hide(status_wifi_);
        Hide(status_battery_);
        return;
    }
    RefreshStatusRow();
    Show(status_time_);
    Show(status_wifi_);
    Show(status_battery_);
}

bool CoreS3V4Display::IsMenuScreen() const {
    return screen_ == V4Screen::Main || screen_ == V4Screen::Listening ||
           screen_ == V4Screen::Speaking;
}

bool CoreS3V4Display::IsClockText(const char* status) const {
    return status != nullptr && strlen(status) == 5 && status[2] == ':' &&
           isdigit(static_cast<unsigned char>(status[0])) &&
           isdigit(static_cast<unsigned char>(status[1])) &&
           isdigit(static_cast<unsigned char>(status[3])) &&
           isdigit(static_cast<unsigned char>(status[4]));
}

void CoreS3V4Display::EnterBrowseMode() {
    menu_mode_ = MenuMode::Browse;
    wifi_confirm_yes_ = false;
}

void CoreS3V4Display::ShowOverlayText(const char* title, const char* subtitle, const char* body) {
    Hide(nexus_logo_);
    Hide(listening_label_);
    Hide(emoji_box_);
    Hide(ready_check_);
    Hide(qr_image_);
    Hide(icon_label_);
    ShowStatusRow(false);

    auto set_line = [&](lv_obj_t* label, const char* text, int y) {
        if (label == nullptr) {
            return;
        }
        lv_label_set_text(label, text != nullptr ? text : "");
        lv_obj_set_style_text_font(label, TextFont(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_width(label, 240);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(label, 136, y);
        if (text != nullptr && text[0] != '\0') {
            Show(label);
        } else {
            Hide(label);
        }
    };
    set_line(title_label_, title, 90);
    set_line(subtitle_label_, subtitle, 130);
    set_line(body_label_, body, 168);
    ApplyMenuSelectionVisuals();
}

void CoreS3V4Display::ApplyWifiConfirmOptionVisuals() {
    if (subtitle_label_ != nullptr) {
        lv_obj_set_style_text_opa(subtitle_label_, wifi_confirm_yes_ ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    if (body_label_ != nullptr) {
        lv_obj_set_style_text_opa(body_label_, wifi_confirm_yes_ ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

void CoreS3V4Display::ShowConversationVolumeNumber() {
    auto codec = Board::GetInstance().GetAudioCodec();
    const int vol = codec != nullptr ? codec->output_volume() : 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", vol);
    Hide(title_label_);
    Hide(subtitle_label_);
    if (body_label_ != nullptr) {
        lv_obj_set_style_text_font(body_label_, TextFont(), 0);
        lv_label_set_text(body_label_, buf);
        lv_obj_set_style_text_color(body_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_opa(body_label_, LV_OPA_COVER, 0);
        lv_obj_set_width(body_label_, LV_SIZE_CONTENT);
        PlaceCenter(body_label_, 136, 200);
        Show(body_label_);
        lv_obj_move_foreground(body_label_);
    }
    ApplyMenuSelectionVisuals();
}

void CoreS3V4Display::RefreshVolumeOverlay() {
    if (screen_ == V4Screen::Listening || screen_ == V4Screen::Speaking) {
        ShowConversationVolumeNumber();
        return;
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    const int vol = codec != nullptr ? codec->output_volume() : 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", vol);
    ShowOverlayText("Volume", buf, nullptr);
}

void CoreS3V4Display::ShowTasksPlaceholder() {
    menu_mode_ = MenuMode::TasksPlaceholder;
    menu_selected_ = MenuItem::Tasks;
    ShowOverlayText("Tasks", "Coming soon", nullptr);
}

void CoreS3V4Display::ShowVolumeAdjustMode() {
    menu_mode_ = MenuMode::VolumeAdjust;
    menu_selected_ = MenuItem::Volume;
    RefreshVolumeOverlay();
}

void CoreS3V4Display::ShowWifiConfirmMode() {
    menu_mode_ = MenuMode::WifiConfirm;
    menu_selected_ = MenuItem::ChangeWifi;
    wifi_confirm_yes_ = false;
    ShowOverlayText("CHANGE WI-FI?", "No", "Yes");
    ApplyWifiConfirmOptionVisuals();
}

void CoreS3V4Display::ReturnFromOverlay() {
    menu_mode_ = MenuMode::Browse;
    wifi_confirm_yes_ = false;
    Hide(body_label_);
    Hide(title_label_);
    Hide(subtitle_label_);
    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateListening) {
        ShowListeningScreen();
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateNotifying) {
        ShowSpeakingScreen();
    } else {
        ShowMainScreen();
    }
}

void CoreS3V4Display::MoveMenuSelection(bool next) {
    int idx = static_cast<int>(menu_selected_);
    const int count = static_cast<int>(MenuItem::Count);
    idx = next ? (idx + 1) % count : (idx - 1 + count) % count;
    menu_selected_ = static_cast<MenuItem>(idx);
    ApplyMenuSelectionVisuals();
    ESP_LOGI(TAG, "Menu select %d", idx);
}

void CoreS3V4Display::AdjustSpeakerVolume(bool increase) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int current = codec->output_volume();
    int next = current + (increase ? kVolumeStep : -kVolumeStep);
    if (next > 100) {
        next = 100;
    } else if (next < 0) {
        next = 0;
    }
    codec->SetOutputVolume(next);
    ESP_LOGI(TAG, "Volume %d -> %d", current, next);
    RefreshVolumeOverlay();
}

void CoreS3V4Display::ActivateSelectedMenuItem() {
    switch (menu_selected_) {
        case MenuItem::Talk: {
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateIdle || state == kDeviceStateListening ||
                state == kDeviceStateSpeaking || state == kDeviceStateNotifying) {
                Application::GetInstance().ToggleChatState();
            } else {
                ESP_LOGI(TAG, "Talk ignored in device state %d", static_cast<int>(state));
            }
            break;
        }
        case MenuItem::Tasks:
            ShowTasksPlaceholder();
            break;
        case MenuItem::Volume:
            ShowVolumeAdjustMode();
            break;
        case MenuItem::ChangeWifi:
            ShowWifiConfirmMode();
            break;
        default:
            break;
    }
}

bool CoreS3V4Display::ConfirmChangeWifi() {
    {
        Settings app_settings("app", true);
        app_settings.SetBool("cwifi_pend", true);
    }
    {
        Settings app_settings("app", false);
        if (!app_settings.GetBool("cwifi_pend", false)) {
            ESP_LOGE(TAG, "Change Wi-Fi: failed to persist cwifi_pend marker — aborting");
            DisplayLockGuard lock(this);
            ReturnFromOverlay();
            return true;
        }
    }

    auto& ssids = SsidManager::GetInstance();
    ssids.Clear();
    if (!ssids.GetSsidList().empty()) {
        ESP_LOGE(TAG, "Change Wi-Fi: SsidManager::Clear() left %u network(s)",
                 static_cast<unsigned>(ssids.GetSsidList().size()));
        Settings app_settings("app", true);
        app_settings.SetBool("cwifi_pend", false);
        DisplayLockGuard lock(this);
        ReturnFromOverlay();
        return true;
    }
    ESP_LOGW(TAG, "Change Wi-Fi: saved STA credentials cleared; rebooting");
    Application::GetInstance().Reboot();
    return true;
}

void CoreS3V4Display::ShowBootInitializingScreen() {
    screen_ = V4Screen::Boot;
    ready_splash_active_ = false;
    menu_mode_ = MenuMode::Inactive;
    ShowV4Layer();
    HideAllContent();
    if (nexus_logo_ != nullptr) {
        lv_label_set_text(nexus_logo_, "NEXUS");
        lv_obj_set_style_text_color(nexus_logo_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_letter_space(nexus_logo_, 4, 0);
        PlaceCenter(nexus_logo_, 160, 100);
        Show(nexus_logo_);
    }
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, Lang::Strings::INITIALIZING);
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(subtitle_label_, LV_SIZE_CONTENT);
        PlaceCenter(subtitle_label_, 160, 140);
        Show(subtitle_label_);
    }
}

void CoreS3V4Display::ShowProvisionQrScreen() {
    screen_ = V4Screen::ProvisionQr;
    ready_splash_active_ = false;
    menu_mode_ = MenuMode::Inactive;
    ShowV4Layer();
    HideAllContent();
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, "CONNECT TO NEXUS");
        lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(title_label_, TextFont(), 0);
        lv_obj_set_width(title_label_, 250);
        lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(title_label_, 136, 18);
        Show(title_label_);
    }
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, "Scan to begin setup");
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(subtitle_label_, TextFont(), 0);
        lv_obj_set_width(subtitle_label_, 250);
        lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(subtitle_label_, 136, 44);
        Show(subtitle_label_);
    }
    if (qr_image_ != nullptr) {
        // Native 165x165 I1 image. LVGL cannot scale indexed images.
        lv_image_set_src(qr_image_, &nexus_wifi_qr);
        lv_image_set_scale(qr_image_, LV_SCALE_NONE);
        lv_image_set_antialias(qr_image_, false);
        lv_obj_set_pos(qr_image_, 50, 68);
        Show(qr_image_);
    }
}

void CoreS3V4Display::ShowWaitingWifiScreen() {
    screen_ = V4Screen::WaitingWifi;
    ready_splash_active_ = false;
    menu_mode_ = MenuMode::Inactive;
    ShowV4Layer();
    HideAllContent();
    if (icon_label_ != nullptr) {
        lv_label_set_text(icon_label_, MATERIAL_SYMBOLS_WIFI);
        lv_obj_set_style_text_font(icon_label_, LargeIconFont(), 0);
        lv_obj_set_style_text_color(icon_label_, lv_color_hex(kLime), 0);
        PlaceCenter(icon_label_, 136, 70);
        Show(icon_label_);
    }
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, "Waiting for Wi-Fi");
        lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(title_label_, 250);
        lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(title_label_, 136, 130);
        Show(title_label_);
    }
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, "Setup...");
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
        PlaceCenter(subtitle_label_, 136, 162);
        Show(subtitle_label_);
    }
    if (body_label_ != nullptr) {
        lv_label_set_text(body_label_, "http://192.168.4.1");
        lv_obj_set_style_text_color(body_label_, lv_color_hex(kLime), 0);
        PlaceCenter(body_label_, 136, 196);
        Show(body_label_);
    }
}

void CoreS3V4Display::ShowBindingScreen(bool mark_real_binding) {
    screen_ = V4Screen::Binding;
    if (mark_real_binding) {
        binding_ui_shown_ = true;
    }
    ready_splash_active_ = false;
    menu_mode_ = MenuMode::Inactive;
    ShowV4Layer();
    HideAllContent();
    if (icon_label_ != nullptr) {
        lv_label_set_text(icon_label_, MATERIAL_SYMBOLS_WIFI);
        lv_obj_set_style_text_font(icon_label_, LargeIconFont(), 0);
        lv_obj_set_style_text_color(icon_label_, lv_color_hex(kLime), 0);
        PlaceCenter(icon_label_, 136, 64);
        Show(icon_label_);
    }
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, "Wi-Fi Connected");
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
        PlaceCenter(subtitle_label_, 136, 120);
        Show(subtitle_label_);
    }
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, "Binding device to");
        lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(title_label_, 250);
        lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(title_label_, 136, 156);
        Show(title_label_);
    }
    if (body_label_ != nullptr) {
        lv_label_set_text(body_label_, "your account");
        lv_obj_set_style_text_color(body_label_, lv_color_hex(0xFFFFFF), 0);
        PlaceCenter(body_label_, 136, 190);
        Show(body_label_);
    }
}

void CoreS3V4Display::ReadySplashTimerCallback(void* arg) {
    auto self = static_cast<CoreS3V4Display*>(arg);
    DisplayLockGuard lock(self);
    self->ready_splash_active_ = false;
    self->ShowMainScreen();
}

void CoreS3V4Display::ShowReadySplashScreen() {
    screen_ = V4Screen::ReadySplash;
    ready_splash_active_ = true;
    menu_mode_ = MenuMode::Inactive;
    binding_ui_shown_ = false;
    ShowV4Layer();
    HideAllContent();
    Show(ready_check_);
    if (title_label_ != nullptr) {
        lv_label_set_text(title_label_, "Nexus is ready!");
        lv_obj_set_style_text_color(title_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(title_label_, 250);
        lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
        PlaceCenter(title_label_, 160, 168);
        Show(title_label_);
    }
    if (ready_timer_ != nullptr) {
        esp_timer_stop(ready_timer_);
        esp_timer_start_once(ready_timer_, kReadySplashUs);
    }
}

void CoreS3V4Display::ShowMainScreen() {
    screen_ = V4Screen::Main;
    ready_splash_active_ = false;
    EnterBrowseMode();
    ShowV4Layer();
    HideAllContent();
    ShowStatusRow(true);
    if (nexus_logo_ != nullptr) {
        lv_label_set_text(nexus_logo_, "NEXUS");
        lv_obj_set_style_text_letter_space(nexus_logo_, 4, 0);
        lv_obj_set_style_text_color(nexus_logo_, lv_color_hex(0xFFFFFF), 0);
        PlaceCenter(nexus_logo_, 136, 120);
        Show(nexus_logo_);
    }
    ApplyMenuSelectionVisuals();
}

void CoreS3V4Display::ShowListeningScreen() {
    screen_ = V4Screen::Listening;
    ready_splash_active_ = false;
    const bool keep_volume = menu_mode_ == MenuMode::VolumeAdjust;
    const bool keep_modal =
        menu_mode_ == MenuMode::TasksPlaceholder || menu_mode_ == MenuMode::WifiConfirm;
    if (!keep_volume && !keep_modal) {
        EnterBrowseMode();
    }
    ShowV4Layer();
    if (keep_modal) {
        ApplyMenuSelectionVisuals();
        return;
    }
    HideAllContent();
    ShowStatusRow(true);
    if (listening_label_ != nullptr) {
        lv_label_set_text(listening_label_, "Listening");
        lv_obj_set_style_text_color(listening_label_, lv_color_hex(0xFFB300), 0);
        PlaceCenter(listening_label_, 136, 48);
        Show(listening_label_);
    }
    SpiLcdDisplay::SetEmotion("happy");
    if (emoji_box_ != nullptr) {
        PlaceCenter(emoji_box_, 136, 130);
        Show(emoji_box_);
    }
    if (keep_volume) {
        ShowConversationVolumeNumber();
    } else {
        ApplyMenuSelectionVisuals();
    }
}

void CoreS3V4Display::ShowSpeakingScreen() {
    screen_ = V4Screen::Speaking;
    ready_splash_active_ = false;
    const bool keep_volume = menu_mode_ == MenuMode::VolumeAdjust;
    const bool keep_modal =
        menu_mode_ == MenuMode::TasksPlaceholder || menu_mode_ == MenuMode::WifiConfirm;
    if (!keep_volume && !keep_modal) {
        EnterBrowseMode();
    }
    ShowV4Layer();
    if (keep_modal) {
        ApplyMenuSelectionVisuals();
        return;
    }
    HideAllContent();
    ShowStatusRow(true);
    if (listening_label_ != nullptr) {
        lv_label_set_text(listening_label_, "Speaking");
        lv_obj_set_style_text_color(listening_label_, lv_color_hex(0xFFB300), 0);
        PlaceCenter(listening_label_, 136, 48);
        Show(listening_label_);
    }
    if (emoji_box_ != nullptr) {
        PlaceCenter(emoji_box_, 136, 130);
        Show(emoji_box_);
    }
    if (keep_volume) {
        ShowConversationVolumeNumber();
    } else {
        ApplyMenuSelectionVisuals();
    }
}

void CoreS3V4Display::RestoreScreenForDeviceState() {
    auto state = Application::GetInstance().GetDeviceState();
    switch (state) {
        case kDeviceStateStarting:
            ShowBootInitializingScreen();
            break;
        case kDeviceStateWifiConfiguring:
            ShowProvisionQrScreen();
            break;
        case kDeviceStateConnecting:
            if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
                screen_ == V4Screen::Binding) {
                ShowWaitingWifiScreen();
            } else {
                ShowMainScreen();
            }
            break;
        case kDeviceStateActivating:
            if (Application::GetInstance().ShouldSuppressBindingUi()) {
                ShowWaitingWifiScreen();
            } else {
                ShowBindingScreen(binding_ui_shown_);
            }
            break;
        case kDeviceStateListening:
            ShowListeningScreen();
            break;
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            ShowSpeakingScreen();
            break;
        case kDeviceStateIdle:
        default:
            if (ready_splash_active_) {
                ShowReadySplashScreen();
            } else {
                ShowMainScreen();
            }
            break;
    }
}

void CoreS3V4Display::RebindFonts() {
    const lv_font_t* text_font = TextFont();
    const lv_font_t* icon_font = IconFont();
    lv_obj_t* text_objs[] = {title_label_,     subtitle_label_, body_label_,      nexus_logo_,
                             listening_label_, status_time_,    low_battery_label_};
    for (lv_obj_t* obj : text_objs) {
        if (obj != nullptr && text_font != nullptr) {
            lv_obj_set_style_text_font(obj, text_font, 0);
        }
    }
    lv_obj_t* icon_objs[] = {icon_label_, status_wifi_, status_battery_, icon_talk_,
                             icon_task_,  icon_volume_, icon_wifi_};
    for (lv_obj_t* obj : icon_objs) {
        if (obj != nullptr && icon_font != nullptr) {
            lv_obj_set_style_text_font(obj, icon_font, 0);
        }
    }
}

void CoreS3V4Display::StyleLowBattery() {
    if (low_battery_popup_ == nullptr || low_battery_label_ == nullptr) {
        return;
    }
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 0, 0);
    lv_obj_set_style_radius(low_battery_popup_, 0, 0);
    lv_label_set_text(low_battery_label_, "BATTERY LOW");
    lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(0xF01818), 0);
    lv_obj_set_style_text_font(low_battery_label_, TextFont(), 0);
    lv_obj_set_style_text_align(low_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(low_battery_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(low_battery_popup_);
}

void CoreS3V4Display::SetPanelDisplayOn(bool on) {
    if (panel_ == nullptr) {
        return;
    }
    esp_err_t err = esp_lcd_panel_disp_on_off(panel_, on);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Panel does not support disp_on_off; backlight-only screen-off");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_disp_on_off(%s) failed: %s", on ? "on" : "off",
                 esp_err_to_name(err));
    }
}

bool CoreS3V4Display::OnVerticalSwipe(bool finger_moved_down) {
    const int64_t now = esp_timer_get_time();
    if (now - last_swipe_us_ < kSwipeDebounceUs) {
        return menu_mode_ != MenuMode::Inactive;
    }
    last_swipe_us_ = now;

    DisplayLockGuard lock(this);
    switch (menu_mode_) {
        case MenuMode::Browse:
            if (!IsMenuScreen()) {
                return false;
            }
            // Finger down moves the highlight down the column.
            MoveMenuSelection(finger_moved_down);
            return true;
        case MenuMode::VolumeAdjust:
            // Finger up raises the volume.
            AdjustSpeakerVolume(!finger_moved_down);
            return true;
        case MenuMode::WifiConfirm:
            wifi_confirm_yes_ = !wifi_confirm_yes_;
            ApplyWifiConfirmOptionVisuals();
            ESP_LOGI(TAG, "Change Wi-Fi confirm highlight: %s", wifi_confirm_yes_ ? "Yes" : "No");
            return true;
        case MenuMode::TasksPlaceholder:
            return true;
        case MenuMode::Inactive:
        default:
            return false;
    }
}

bool CoreS3V4Display::OnTap(int x, int y) {
    bool clear_wifi_and_reboot = false;
    {
        DisplayLockGuard lock(this);
        if (menu_mode_ == MenuMode::Browse && IsMenuScreen() && x >= kIconColumnX) {
            int row = y / kRowHeight;
            if (row < 0) {
                row = 0;
            }
            if (row >= static_cast<int>(MenuItem::Count)) {
                row = static_cast<int>(MenuItem::Count) - 1;
            }
            menu_selected_ = static_cast<MenuItem>(row);
            ApplyMenuSelectionVisuals();
            ActivateSelectedMenuItem();
            return true;
        }

        switch (menu_mode_) {
            case MenuMode::Browse:
                if (!IsMenuScreen()) {
                    return false;
                }
                ActivateSelectedMenuItem();
                return true;
            case MenuMode::VolumeAdjust:
                ESP_LOGI(TAG, "Volume adjust exit");
                ReturnFromOverlay();
                return true;
            case MenuMode::TasksPlaceholder:
                ESP_LOGI(TAG, "Tasks placeholder exit");
                ReturnFromOverlay();
                return true;
            case MenuMode::WifiConfirm:
                if (wifi_confirm_yes_) {
                    clear_wifi_and_reboot = true;
                    break;
                }
                ESP_LOGI(TAG, "Change Wi-Fi confirmation: No — cancelled");
                ReturnFromOverlay();
                return true;
            case MenuMode::Inactive:
            default:
                return false;
        }
    }
    if (clear_wifi_and_reboot) {
        return ConfirmChangeWifi();
    }
    return true;
}

void CoreS3V4Display::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    v4_layer_ = lv_obj_create(screen);
    lv_obj_remove_style_all(v4_layer_);
    lv_obj_set_pos(v4_layer_, 0, 0);
    lv_obj_set_size(v4_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(v4_layer_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(v4_layer_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(v4_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(v4_layer_);

    if (emoji_box_ != nullptr) {
        lv_obj_set_parent(emoji_box_, v4_layer_);
        Hide(emoji_box_);
    }
    if (low_battery_popup_ != nullptr) {
        lv_obj_set_parent(low_battery_popup_, v4_layer_);
    }

    const lv_font_t* text_font = TextFont();
    const lv_font_t* icon_font = IconFont();
    title_label_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
    subtitle_label_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
    body_label_ = MakeLabel(v4_layer_, text_font, kLime);
    icon_label_ = MakeLabel(v4_layer_, icon_font, kLime);

    qr_image_ = lv_image_create(v4_layer_);
    lv_image_set_src(qr_image_, &nexus_wifi_qr);
    lv_image_set_scale(qr_image_, LV_SCALE_NONE);
    lv_image_set_antialias(qr_image_, false);
    lv_obj_add_flag(qr_image_, LV_OBJ_FLAG_HIDDEN);

    status_time_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
    status_wifi_ = MakeLabel(v4_layer_, icon_font, 0xFFFFFF);
    status_battery_ = MakeLabel(v4_layer_, icon_font, 0xFFFFFF);
    PlaceCenter(status_time_, 48, 16);
    PlaceCenter(status_wifi_, 136, 16);
    PlaceCenter(status_battery_, 214, 16);

    ready_check_ = lv_line_create(v4_layer_);
    {
        static lv_point_precise_t check_pts[] = {{10, 24}, {22, 38}, {46, 8}};
        lv_line_set_points(ready_check_, check_pts, 3);
        lv_obj_set_style_line_width(ready_check_, 6, 0);
        lv_obj_set_style_line_color(ready_check_, lv_color_hex(kLime), 0);
        lv_obj_set_style_line_rounded(ready_check_, true, 0);
        lv_obj_set_pos(ready_check_, 118, 78);
        Hide(ready_check_);
    }

    nexus_logo_ = MakeLabel(v4_layer_, text_font, 0xFFFFFF);
    listening_label_ = MakeLabel(v4_layer_, text_font, 0xFFB300);
    icon_talk_ = MakeLabel(v4_layer_, icon_font, kLime);
    icon_task_ = MakeLabel(v4_layer_, icon_font, kLime);
    icon_volume_ = MakeLabel(v4_layer_, icon_font, kLime);
    icon_wifi_ = MakeLabel(v4_layer_, icon_font, kLime);
    lv_label_set_text(icon_talk_, MATERIAL_SYMBOLS_CHAT_BUBBLE);
    lv_label_set_text(icon_task_, MATERIAL_SYMBOLS_CALENDAR_MONTH);
    lv_label_set_text(icon_volume_, MATERIAL_SYMBOLS_VOLUME_UP);
    lv_label_set_text(icon_wifi_, MATERIAL_SYMBOLS_WIFI);
    PlaceCenter(icon_talk_, kIconCenterX, 30);
    PlaceCenter(icon_task_, kIconCenterX, 90);
    PlaceCenter(icon_volume_, kIconCenterX, 150);
    PlaceCenter(icon_wifi_, kIconCenterX, 210);

    esp_timer_create_args_t ready_args = {};
    ready_args.callback = ReadySplashTimerCallback;
    ready_args.arg = this;
    ready_args.dispatch_method = ESP_TIMER_TASK;
    ready_args.name = "v4_ready";
    ESP_ERROR_CHECK(esp_timer_create(&ready_args, &ready_timer_));

    ShowBootInitializingScreen();
    ESP_LOGI(TAG, "V4 UI on 320x240 — swipe selects, tap activates");
}

void CoreS3V4Display::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    if (v4_layer_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    RebindFonts();
    StyleLowBattery();
}

void CoreS3V4Display::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (on) {
        SetPanelDisplayOn(false);
        ShowV4Layer();
    } else {
        SetPanelDisplayOn(true);
        RestoreScreenForDeviceState();
    }
}

void CoreS3V4Display::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    if (v4_layer_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (screen_ == V4Screen::Main || screen_ == V4Screen::Listening ||
        screen_ == V4Screen::Speaking) {
        RefreshStatusRow();
    }
    if (low_battery_popup_ != nullptr && !lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) {
        StyleLowBattery();
    }
}

void CoreS3V4Display::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (status == nullptr || v4_layer_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    if (IsClockText(status)) {
        if (status_time_ != nullptr && screen_ == V4Screen::Main) {
            lv_label_set_text(status_time_, status);
        }
        return;
    }

    if (strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
        strcmp(status, Lang::Strings::ENTERING_WIFI_CONFIG_MODE) == 0) {
        ShowProvisionQrScreen();
        return;
    }
    if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
        if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi) {
            ShowWaitingWifiScreen();
        }
        return;
    }
    if (strcmp(status, Lang::Strings::ACTIVATION) == 0) {
        if (Application::GetInstance().ShouldSuppressBindingUi()) {
            ShowWaitingWifiScreen();
            return;
        }
        ShowBindingScreen(true);
        return;
    }
    if (strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0) {
        if (Application::GetInstance().ShouldSuppressBindingUi()) {
            if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
                screen_ == V4Screen::Binding) {
                ShowWaitingWifiScreen();
            }
            return;
        }
        if (screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
            screen_ == V4Screen::Binding) {
            ShowBindingScreen(false);
        }
        return;
    }
    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        ShowListeningScreen();
        return;
    }
    if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        ShowSpeakingScreen();
        return;
    }
    if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        if (binding_ui_shown_) {
            ShowReadySplashScreen();
        } else if (!ready_splash_active_) {
            ShowMainScreen();
        }
    }
}

void CoreS3V4Display::SetEmotion(const char* emotion) {
    if (v4_layer_ == nullptr) {
        SpiLcdDisplay::SetEmotion(emotion);
        return;
    }
    DisplayLockGuard lock(this);
    if (emotion != nullptr && strcmp(emotion, "sleepy") == 0) {
        return;
    }
    if (screen_ == V4Screen::Listening) {
        SpiLcdDisplay::SetEmotion("happy");
        if (emoji_box_ != nullptr) {
            PlaceCenter(emoji_box_, 136, 130);
            Show(emoji_box_);
        }
        return;
    }
    SpiLcdDisplay::SetEmotion(emotion);
    if (screen_ == V4Screen::Speaking) {
        if (emoji_box_ != nullptr) {
            PlaceCenter(emoji_box_, 136, 130);
            Show(emoji_box_);
        }
        return;
    }
    if (screen_ == V4Screen::Main || screen_ == V4Screen::ReadySplash ||
        screen_ == V4Screen::ProvisionQr || screen_ == V4Screen::WaitingWifi ||
        screen_ == V4Screen::Binding || screen_ == V4Screen::Boot) {
        Hide(emoji_box_);
    }
}

void CoreS3V4Display::SetChatMessage(const char* role, const char* content) {
    if (role != nullptr && content != nullptr && strcmp(role, "system") == 0 &&
        strstr(content, "Hotspot: ") != nullptr) {
        if (v4_layer_ == nullptr) {
            return;
        }
        DisplayLockGuard lock(this);
        ShowProvisionQrScreen();
        return;
    }
    if (role != nullptr && strcmp(role, "system") == 0) {
        return;
    }
    SpiLcdDisplay::SetChatMessage(role, content);
}
