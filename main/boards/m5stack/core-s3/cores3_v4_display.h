#ifndef CORES3_V4_DISPLAY_H_
#define CORES3_V4_DISPLAY_H_

#include "display/lcd_display.h"

#include <esp_timer.h>

// Nexus V4 screens adapted to the CoreS3 320x240 panel.
// Watcher geometry (412x412 arcs) is not used.
class CoreS3V4Display : public SpiLcdDisplay {
public:
    CoreS3V4Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                    int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                    bool swap_xy);
    ~CoreS3V4Display() override;

    // Touch stand-in for the Watcher rotary encoder.
    // finger_moved_down selects the next menu row, lowers volume, or toggles Yes/No.
    // Returns false when the screen is not interactive so the board can keep its fallback.
    bool OnVerticalSwipe(bool finger_moved_down);
    // x/y are FT6336 coordinates. A tap in the right icon column selects and activates
    // that item. A tap elsewhere is the wheel click.
    bool OnTap(int x, int y);
    // Continuous touch move while finger is down (volume slider drag).
    bool OnTouchMove(int x, int y);
    void OnTouchEnd();

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetPowerSaveMode(bool on) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetTheme(Theme* theme) override;

private:
    enum class V4Screen {
        Boot,
        ProvisionQr,
        WaitingWifi,
        Binding,
        ReadySplash,
        Main,
        Listening,
        Speaking,
    };

    // Same order as the Watcher wheel, drawn top to bottom.
    enum class MenuItem : int {
        Talk = 0,
        Tasks = 1,
        Volume = 2,
        ChangeWifi = 3,
        Count = 4,
    };

    enum class MenuMode {
        Inactive,
        Browse,
        VolumeAdjust,
        TasksPlaceholder,
        WifiConfirm,
    };

    static constexpr uint32_t kLime = 0x7CFF14;
    static constexpr uint32_t kStatusRed = 0xF01818;
    static constexpr uint32_t kIconMuted = 0x8B8B8B;
    static constexpr uint32_t kIconSelected = 0xFFFFFF;
    static constexpr int kIconColumnX = 268;
    static constexpr int kIconCenterX = 294;
    static constexpr int kRowHeight = 60;
    static constexpr int kContentShiftX = -22;
    static constexpr int kVolumeStep = 5;
    static constexpr int kReadySplashUs = 4000000;
    static constexpr int64_t kSwipeDebounceUs = 120000;
    // HUD frame: equal 8px margins in the content pane (left of icon column).
    // Content pane usable width ends at kIconColumnX; top clears the status row.
    static constexpr int kFrameLeft = 8;
    static constexpr int kFrameTop = 28;
    static constexpr int kFrameRight = 260;
    static constexpr int kFrameBottom = 232;
    static constexpr int kFrameCorner = 28;
    static constexpr int kFrameLineW = 2;
    static constexpr int kFrameSegCount = 8;
    // Content center inside the frame (symmetrical between L/R).
    static constexpr int kContentCenterX = (kFrameLeft + kFrameRight) / 2;  // 134
    // Agent emoji: 64px asset scaled 2x (LVGL scale 256 = 100%).
    static constexpr int kEmojiScale = 512;
    static constexpr int kEmojiCenterY = 128;
    static constexpr int kStatusLabelY = 46;
    // NEXUS status word: centered between lower corner brackets.
    static constexpr int kNexusStatusY = 214;
    // Provision QR (native 165x165) + URL under it, inside the frame.
    static constexpr int kQrSize = 165;
    static constexpr int kQrX = kContentCenterX - kQrSize / 2;  // 52
    static constexpr int kQrY = 50;
    static constexpr int kQrUrlY = 226;
    // Volume slider geometry (inside HUD frame; left of icon column).
    static constexpr int kSliderX = 48;
    static constexpr int kSliderY = 118;
    static constexpr int kSliderW = 176;
    static constexpr int kSliderH = 22;
    static constexpr int kSliderHitPadY = 18;
    static constexpr int kSliderLabelY = 78;
    static constexpr int kSliderPercentY = 168;
    // Change Wi-Fi overlay lines (ShowOverlayText centers): No @ y=130, Yes @ y=168.
    static constexpr int kWifiConfirmNoCenterY = 130;
    static constexpr int kWifiConfirmYesCenterY = 168;
    // Split midway between No and Yes centers so each option has a direct hit band.
    static constexpr int kWifiConfirmSplitY = (kWifiConfirmNoCenterY + kWifiConfirmYesCenterY) / 2;  // 149
    static constexpr int kWifiConfirmNoTopY = 110;
    static constexpr int kWifiConfirmYesBottomY = 190;

    lv_obj_t* v4_layer_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    lv_obj_t* body_label_ = nullptr;
    lv_obj_t* icon_label_ = nullptr;
    lv_obj_t* qr_image_ = nullptr;
    lv_obj_t* status_time_ = nullptr;
    lv_obj_t* status_wifi_ = nullptr;
    lv_obj_t* status_battery_ = nullptr;
    lv_obj_t* nexus_logo_ = nullptr;
    lv_obj_t* listening_label_ = nullptr;
    lv_obj_t* icon_talk_ = nullptr;
    lv_obj_t* icon_task_ = nullptr;
    lv_obj_t* icon_volume_ = nullptr;
    lv_obj_t* icon_wifi_ = nullptr;
    lv_obj_t* ready_check_ = nullptr;
    lv_obj_t* hud_frame_ = nullptr;
    lv_obj_t* frame_lines_[kFrameSegCount] = {};
    lv_point_precise_t frame_pts_[kFrameSegCount][2] = {};
    lv_obj_t* volume_slider_ = nullptr;

    esp_timer_handle_t ready_timer_ = nullptr;
    bool binding_ui_shown_ = false;
    bool ready_splash_active_ = false;
    bool wifi_confirm_yes_ = false;
    bool volume_slider_dragging_ = false;
    V4Screen screen_ = V4Screen::Boot;
    MenuItem menu_selected_ = MenuItem::Talk;
    MenuMode menu_mode_ = MenuMode::Inactive;
    int64_t last_swipe_us_ = 0;

    const lv_font_t* TextFont() const;
    const lv_font_t* IconFont() const;
    const lv_font_t* LargeIconFont() const;

    lv_obj_t* MakeLabel(lv_obj_t* parent, const lv_font_t* font, uint32_t color);
    void PlaceCenter(lv_obj_t* obj, int cx, int cy);
    void Hide(lv_obj_t* obj);
    void Show(lv_obj_t* obj);
    void HideAllContent();
    void ShowV4Layer();
    void CreateHudFrame();
    void ShowHudFrame(bool visible);
    void ApplyMenuSelectionVisuals();
    void RefreshStatusRow();
    void ShowStatusRow(bool visible);
    void RefreshWifiStatusColor();
    void RefreshNexusStatusColor();
    void ShowNexusStatusIndicator(bool visible);
    void ApplyAgentEmojiLayout();
    bool IsLiveWifiConnected() const;
    bool IsMenuScreen() const;
    bool IsClockText(const char* status) const;
    void EnterBrowseMode();
    void ShowOverlayText(const char* title, const char* subtitle, const char* body);
    void ApplyWifiConfirmOptionVisuals();
    void ShowConversationVolumeNumber();
    void RefreshVolumeOverlay();
    void CreateVolumeSlider();
    void ShowVolumeSlider(bool visible);
    void SyncVolumeSliderFromCodec();
    bool HitTestVolumeSlider(int x, int y) const;
    bool ApplyVolumeFromSliderX(int x);
    void ShowTasksPlaceholder();
    void ShowVolumeAdjustMode();
    void ShowWifiConfirmMode();
    void ReturnFromOverlay();
    void MoveMenuSelection(bool next);
    void AdjustSpeakerVolume(bool increase);
    void ActivateSelectedMenuItem();
    bool ConfirmChangeWifi();

    void ShowBootInitializingScreen();
    void ShowProvisionQrScreen();
    void ShowWaitingWifiScreen();
    void ShowBindingScreen(bool mark_real_binding);
    void ShowReadySplashScreen();
    void ShowMainScreen();
    void ShowListeningScreen();
    void ShowSpeakingScreen();
    void RestoreScreenForDeviceState();
    void RebindFonts();
    void StyleLowBattery();
    void SetPanelDisplayOn(bool on);

    static void ReadySplashTimerCallback(void* arg);
};

#endif  // CORES3_V4_DISPLAY_H_
