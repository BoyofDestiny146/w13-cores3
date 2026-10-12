#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "cube_internal_audio_codec.h"
#include "codecs/es8388_audio_codec.h"
#include "m144_linemic_route.h"
#include "m144_es7210_isolation.h"
#include "cores3_v4_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "axp2101.h"

#include <cstring>
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_ili9341.h>
#include <esp_timer.h>
#include "esp_video.h"

#define TAG "M5StackCoreS3Board"

#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
namespace {

// Cube V1.0 Variant A — External Audio (M144 ES8388 Config B).
// Keep Es8388 duplex playback; after ADC open, force LIN2 per official
// M5Module-Audio i2sloopback (setADCInput(ADC_INPUT_LINPUT2_RINPUT2)).
// Never constructs CoreS3AudioCodec / ES7210 / AW88298.
class M144Es8388AudioCodec : public Es8388AudioCodec {
public:
    using Es8388AudioCodec::Es8388AudioCodec;

    void EnableInput(bool enable) override {
        Es8388AudioCodec::EnableInput(enable);
        if (enable) {
            SelectMicInputLin2();
        }
    }
};

// Fail-safe stub when ES7210 SDOUT1 Hi-Z is not confirmed: no I2S GPIO claim,
// silence in / discard out — prevents GPIO14 dual-driver contention.
class M144FailSafeAudioCodec : public AudioCodec {
public:
    M144FailSafeAudioCodec(int input_sample_rate, int output_sample_rate) {
        duplex_ = true;
        input_reference_ = false;
        input_channels_ = 1;
        output_channels_ = 1;
        input_sample_rate_ = input_sample_rate;
        output_sample_rate_ = output_sample_rate;
        ESP_LOGE(TAG,
                 "M144FailSafeAudioCodec active — EXTERNAL I2S/ES8388 withheld "
                 "(isolation not confirmed)");
    }

    void SetOutputVolume(int volume) override { AudioCodec::SetOutputVolume(volume); }

    void EnableInput(bool enable) override {
        if (enable) {
            ESP_LOGE(TAG, "fail-safe: EnableInput ignored (no GPIO14 claim)");
        }
        AudioCodec::EnableInput(enable);
    }

    void EnableOutput(bool enable) override {
        if (enable) {
            ESP_LOGE(TAG, "fail-safe: EnableOutput ignored (no ES8388 path)");
        }
        AudioCodec::EnableOutput(enable);
    }

protected:
    int Read(int16_t* dest, int samples) override {
        if (dest != nullptr && samples > 0) {
            std::memset(dest, 0, static_cast<size_t>(samples) * sizeof(int16_t));
        }
        return samples;
    }

    int Write(const int16_t* data, int samples) override {
        (void)data;
        return samples;
    }
};

}  // namespace
#endif  // CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL

class Pmic : public Axp2101 {
public:
    // Power Init
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        uint8_t data = ReadReg(0x90);
        data |= 0b10110100;
        WriteReg(0x90, data);
        WriteReg(0x99, (0b11110 - 5));
        WriteReg(0x97, (0b11110 - 2));
        WriteReg(0x69, 0b00110101);
        WriteReg(0x30, 0b111111);
        WriteReg(0x90, 0xBF);
        WriteReg(0x94, 33 - 5);
        WriteReg(0x95, 33 - 5);
    }

    void SetBrightness(uint8_t brightness) {
        brightness = ((brightness + 641) >> 5);
        WriteReg(0x99, brightness);
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(Pmic *pmic) : pmic_(pmic) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        pmic_->SetBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    Pmic *pmic_;
};

class Aw9523 : public I2cDevice {
public:
    // Expand IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // EXTERNAL: hold AW88298 AW_RST (P0.2) LOW for the session.
        // Keep TOUCH_RST (P0.0) + BUS_OUT_EN (P0.1) high for touch / M-BUS 5V gate.
        WriteReg(0x02, 0b00000011);  // P0: AW_RST=0
#else
        WriteReg(0x02, 0b00000111);  // P0: stock — AW_RST released
#endif
        WriteReg(0x03, 0b10001111);  // P1: includes BOOST_EN (P1.7)
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetAw88298() {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // EXTERNAL must never release AW_RST — Class-D stays in hardware power-down.
        ESP_LOGW(TAG, "ResetAw88298 ignored on EXTERNAL (AW_RST held low)");
        WriteReg(0x02, 0b00000011);
#else
        ESP_LOGI(TAG, "Reset AW88298");
        WriteReg(0x02, 0b00000011);
        vTaskDelay(pdMS_TO_TICKS(10));
        WriteReg(0x02, 0b00000111);
        vTaskDelay(pdMS_TO_TICKS(50));
#endif
    }

#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    uint8_t ReadPort0() { return ReadReg(0x02); }

    void HoldAw88298InReset() {
        WriteReg(0x02, 0b00000011);
    }
#endif

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };
    
    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_;
    Aw9523* aw9523_;
    Ft6336* ft6336_;
    CoreS3V4Display* display_;
    EspVideo* camera_;
    esp_timer_handle_t touchpad_timer_;
    PowerSaveTimer* power_save_timer_;
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    bool m144_isolation_ok_ = false;
    bool m144_stm32_route_ok_ = false;
#endif

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(10);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            pmic_->PowerOff();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void I2cDetect() {
        uint8_t address;
        printf("     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f\r\n");
        for (int i = 0; i < 128; i += 16) {
            printf("%02x: ", i);
            for (int j = 0; j < 16; j++) {
                fflush(stdout);
                address = i + j;
                esp_err_t ret = i2c_master_probe(i2c_bus_, address, pdMS_TO_TICKS(200));
                if (ret == ESP_OK) {
                    printf("%02x ", address);
                } else if (ret == ESP_ERR_TIMEOUT) {
                    printf("UU ");
                } else {
                    printf("-- ");
                }
            }
            printf("\r\n");
        }
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    void PollTouchpad() {
        static bool was_touched = false;
        static int64_t touch_start_time = 0;
        static int start_x = 0;
        static int start_y = 0;
        static int last_x = 0;
        static int last_y = 0;
        const int64_t TOUCH_THRESHOLD_MS = 500;
        const int SWIPE_THRESHOLD_PX = 28;

        ft6336_->UpdateTouchPoint();
        auto& touch_point = ft6336_->GetTouchPoint();

        if (touch_point.num > 0 && !was_touched) {
            was_touched = true;
            touch_start_time = esp_timer_get_time() / 1000;
            start_x = last_x = touch_point.x;
            start_y = last_y = touch_point.y;
        } else if (touch_point.num > 0 && was_touched) {
            last_x = touch_point.x;
            last_y = touch_point.y;
            // Feed continuous moves so the volume slider can track a drag.
            if (display_ != nullptr) {
                display_->OnTouchMove(last_x, last_y);
            }
        } else if (touch_point.num == 0 && was_touched) {
            was_touched = false;
            int64_t touch_duration = (esp_timer_get_time() / 1000) - touch_start_time;
            int dx = last_x - start_x;
            int dy = last_y - start_y;
            int abs_dx = dx < 0 ? -dx : dx;
            int abs_dy = dy < 0 ? -dy : dy;

            if (display_ != nullptr) {
                display_->OnTouchEnd();
            }

            // Vertical swipe replaces the Watcher rotary encoder.
            // Skip swipe-as-volume-step when the gesture was a horizontal slider drag.
            const bool likely_slider_drag = abs_dx >= SWIPE_THRESHOLD_PX && abs_dx >= abs_dy;
            if (!likely_slider_drag && abs_dy >= SWIPE_THRESHOLD_PX && abs_dy > abs_dx &&
                touch_duration < 1500) {
                display_->OnVerticalSwipe(dy > 0);
                return;
            }

            if (touch_duration < TOUCH_THRESHOLD_MS) {
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    EnterWifiConfigMode();
                    return;
                }
                // Tap in the icon column selects that item; a tap elsewhere is the wheel click.
                // Unhandled taps keep the previous chat toggle.
                // For slider taps/drags, use the release point so volume hits stick.
                const int tap_x = likely_slider_drag ? last_x : start_x;
                const int tap_y = likely_slider_drag ? last_y : start_y;
                if (display_ == nullptr || !display_->OnTap(tap_x, tap_y)) {
                    app.ToggleChatState();
                }
            }
        }
    }

    void InitializeFt6336TouchPad() {
        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(i2c_bus_, 0x38);
        
        // 创建定时器，20ms 间隔
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                M5StackCoreS3Board* board = (M5StackCoreS3Board*)arg;
                board->PollTouchpad();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };
        
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &touchpad_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(touchpad_timer_, 20 * 1000));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_37;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_36;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeIli9342Display() {
        ESP_LOGI(TAG, "Init IlI9342");

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_3;
        io_config.dc_gpio_num = GPIO_NUM_35;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        aw9523_->ResetIli9342();

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new CoreS3V4Display(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                       DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                       DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

     void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

public:
    M5StackCoreS3Board() {
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializeAxp2101();
        InitializeAw9523();
        I2cDetect();
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // Step 2A/3A EXTERNAL startup order (before any I2S RX on GPIO14):
        // I2C + AXP + AW9523(AW_RST low) → ES7210 Hi-Z → STM32 route → ES8388.
        ESP_LOGI("M144ExtInit",
                 "variant=EXTERNAL ConfigB MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d "
                 "I2C SDA=%d SCL=%d ES8388=0x10 STM32=0x33 rate_in=%d rate_out=%d",
                 static_cast<int>(M144_AUDIO_I2S_GPIO_MCLK),
                 static_cast<int>(M144_AUDIO_I2S_GPIO_BCLK),
                 static_cast<int>(M144_AUDIO_I2S_GPIO_WS),
                 static_cast<int>(M144_AUDIO_I2S_GPIO_DOUT),
                 static_cast<int>(M144_AUDIO_I2S_GPIO_DIN),
                 static_cast<int>(AUDIO_CODEC_I2C_SDA_PIN),
                 static_cast<int>(AUDIO_CODEC_I2C_SCL_PIN), AUDIO_INPUT_SAMPLE_RATE,
                 AUDIO_OUTPUT_SAMPLE_RATE);

        aw9523_->HoldAw88298InReset();
        const uint8_t port0 = aw9523_->ReadPort0();
        const bool aw_rst_low = (port0 & 0b00000100) == 0;
        ESP_LOGI("M144ExtInit", "AW_RST hold: Port0=0x%02X AW_RST_low=%d (P0.2 clear)", port0,
                 aw_rst_low ? 1 : 0);
        if (!aw_rst_low) {
            ESP_LOGW("M144ExtInit", "AW_RST not low after hold — re-forcing P0.2=0");
            aw9523_->HoldAw88298InReset();
        }
        vTaskDelay(pdMS_TO_TICKS(50));

        m144_isolation_ok_ = Es7210ForceSdout1HiZ(i2c_bus_);
        if (!m144_isolation_ok_) {
            ESP_LOGE("M144ExtInit",
                     "isolation FAIL — skipping STM32 route and ES8388 construction");
        } else {
            // Verified STM32 protocol only: 0x11=CLOSE, 0x00=OPEN (LINE/MIC).
            m144_stm32_route_ok_ = M144EnableLineMicPath(i2c_bus_);
            ESP_LOGI("M144ExtInit", "STM32 LINE/MIC routing ok=%d", m144_stm32_route_ok_ ? 1 : 0);
        }
#endif
        InitializeSpi();
        InitializeIli9342Display();
        InitializeCamera();
        InitializeFt6336TouchPad();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // Variant A — External Audio: M144 ES8388 Config B as I2S slave.
        // MCLK=GPIO7, BCLK=GPIO0, WS=GPIO6, DOUT=13, DIN=14, ES8388=0x10.
        // I2S_NUM_0 owned exclusively by Es8388AudioCodec (never CoreS3AudioCodec).
        if (!m144_isolation_ok_) {
            static M144FailSafeAudioCodec fail_safe(AUDIO_INPUT_SAMPLE_RATE,
                                                    AUDIO_OUTPUT_SAMPLE_RATE);
            return &fail_safe;
        }
        static M144Es8388AudioCodec audio_codec(
            i2c_bus_,
            I2C_NUM_1,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            M144_AUDIO_I2S_GPIO_MCLK,
            M144_AUDIO_I2S_GPIO_BCLK,
            M144_AUDIO_I2S_GPIO_WS,
            M144_AUDIO_I2S_GPIO_DOUT,
            M144_AUDIO_I2S_GPIO_DIN,
            M144_AUDIO_PA_PIN,
            M144_AUDIO_CODEC_ADDR,
            false);
        static bool logged_es8388 = false;
        if (!logged_es8388) {
            logged_es8388 = true;
            ESP_LOGI("M144ExtInit",
                     "ES8388 init path: slave mode ConfigB sample_rate=%d/%d stm32_route=%d",
                     AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                     m144_stm32_route_ok_ ? 1 : 0);
        }
        return &audio_codec;
#elif CONFIG_CUBE_AUDIO_VARIANT_INTERNAL_MIC
#if CONFIG_CUBE_INTERNAL_SPEAKER_BENCH_AW88298
        // TEMPORARY BENCH ONLY — stock CoreS3 AW88298 + ES7210.
        // Not the final Cube V1.0 production speaker attachment.
        // Uses GPIO0 as ES7210 MCLK (compatible). Must NOT be combined with M144 Config B.
        ESP_LOGW(TAG, "Variant B temporary bench: stock AW88298 speaker (not production)");
        static CoreS3AudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
#else
        // Variant B default: built-in ES7210 mic + compile-safe speaker stub.
        // Dedicated speaker pins (CUBE_SPEAKER_*) remain GPIO_NUM_NC until validated.
        // I2S_NUM_0 RX-only for ES7210; no TX GPIO claim; GPIO0 = ES7210 MCLK only.
        static CubeInternalAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
#endif
#else
#error "Unknown Cube audio variant"
        return nullptr;
#endif
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(pmic_);
        return &backlight;
    }
};

DECLARE_BOARD(M5StackCoreS3Board);
