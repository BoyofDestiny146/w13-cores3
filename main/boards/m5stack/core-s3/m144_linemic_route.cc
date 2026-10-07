#include "m144_linemic_route.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "i2c_device.h"

// Distinct tag for bench serial capture of the STM32 routing diagnostic.
#define TAG "M144RouteDiag"

// From m5stack/M5Module-Audio src/M5Module_Audio.h
static constexpr uint8_t kM144Stm32Addr = 0x33;          // I2C_ADDR
static constexpr uint8_t kRegMicrophoneStatus = 0x00;    // MICROPHONE_STATUS (LINE/MIC jack)
static constexpr uint8_t kRegHeadphoneMicStatus = 0x11;  // HEADPHONE_MIC_STATUS
static constexpr uint8_t kAudioMicClose = 0;             // AUDIO_MIC_CLOSE
static constexpr uint8_t kAudioMicOpen = 1;              // AUDIO_MIC_OPEN

namespace {

// Local checked I2C helpers: I2cDevice::WriteReg/ReadReg hide esp_err_t behind
// ESP_ERROR_CHECK. Do not change common/i2c_device.*; capture results here.
class M144Stm32Device : public I2cDevice {
public:
    explicit M144Stm32Device(i2c_master_bus_handle_t i2c_bus) : I2cDevice(i2c_bus, kM144Stm32Addr) {}

    esp_err_t WriteRegChecked(uint8_t reg, uint8_t value) {
        uint8_t buffer[2] = {reg, value};
        return i2c_master_transmit(i2c_device_, buffer, 2, 100);
    }

    esp_err_t ReadRegChecked(uint8_t reg, uint8_t* out) {
        if (out == nullptr) {
            return ESP_ERR_INVALID_ARG;
        }
        return i2c_master_transmit_receive(i2c_device_, &reg, 1, out, 1, 100);
    }
};

}  // namespace

bool M144ProbeController(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "M144 controller probe failed: null I2C bus");
        return false;
    }
    esp_err_t err = i2c_master_probe(i2c_bus, kM144Stm32Addr, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "M144 controller NOT found at I2C 0x%02X (%s / %d)", kM144Stm32Addr,
                 esp_err_to_name(err), static_cast<int>(err));
        return false;
    }
    ESP_LOGI(TAG, "M144 controller found at I2C 0x%02X (probe err=%s / %d)", kM144Stm32Addr,
             esp_err_to_name(err), static_cast<int>(err));
    return true;
}

bool M144EnableLineMicPath(i2c_master_bus_handle_t i2c_bus) {
    if (!M144ProbeController(i2c_bus)) {
        ESP_LOGE(TAG, "LINE/MIC route FAILED: STM32 controller missing");
        return false;
    }

    M144Stm32Device stm32(i2c_bus);

    // --- Diagnostic routing sequence (bench serial: tag M144RouteDiag) ---
    // Keep 0x00=OPEN and do NOT gate mic path on sticky 0x11 (LIN2 policy).

    // 1) Initial read of 0x11 and 0x00
    uint8_t init_11 = 0xFF;
    uint8_t init_00 = 0xFF;
    esp_err_t err_rd_init_11 = stm32.ReadRegChecked(kRegHeadphoneMicStatus, &init_11);
    esp_err_t err_rd_init_00 = stm32.ReadRegChecked(kRegMicrophoneStatus, &init_00);
    ESP_LOGI(TAG,
             "DIAG initial: 0x11=%u (err=%s/%d) 0x00=%u (err=%s/%d)",
             init_11, esp_err_to_name(err_rd_init_11), static_cast<int>(err_rd_init_11),
             init_00, esp_err_to_name(err_rd_init_00), static_cast<int>(err_rd_init_00));

    // 2) Write 0x11 = 0; wait 20 ms; read/log 0x11
    esp_err_t err_wr_11 = stm32.WriteRegChecked(kRegHeadphoneMicStatus, kAudioMicClose);
    ESP_LOGI(TAG, "DIAG write 0x11=%u -> err=%s/%d", kAudioMicClose, esp_err_to_name(err_wr_11),
             static_cast<int>(err_wr_11));
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t post20_11 = 0xFF;
    esp_err_t err_rd_post20_11 = stm32.ReadRegChecked(kRegHeadphoneMicStatus, &post20_11);
    ESP_LOGI(TAG, "DIAG post-write+20ms: 0x11=%u (err=%s/%d)", post20_11,
             esp_err_to_name(err_rd_post20_11), static_cast<int>(err_rd_post20_11));

    // 3) Write 0x00 = 1; wait 20 ms; read/log 0x00
    esp_err_t err_wr_00 = stm32.WriteRegChecked(kRegMicrophoneStatus, kAudioMicOpen);
    ESP_LOGI(TAG, "DIAG write 0x00=%u -> err=%s/%d", kAudioMicOpen, esp_err_to_name(err_wr_00),
             static_cast<int>(err_wr_00));
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t post20_00 = 0xFF;
    esp_err_t err_rd_post20_00 = stm32.ReadRegChecked(kRegMicrophoneStatus, &post20_00);
    ESP_LOGI(TAG, "DIAG post-write+20ms: 0x00=%u (err=%s/%d)", post20_00,
             esp_err_to_name(err_rd_post20_00), static_cast<int>(err_rd_post20_00));

    // 4) Wait another 100 ms; read/log both registers again (FINAL)
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t final_11 = 0xFF;
    uint8_t final_00 = 0xFF;
    esp_err_t err_rd_final_11 = stm32.ReadRegChecked(kRegHeadphoneMicStatus, &final_11);
    esp_err_t err_rd_final_00 = stm32.ReadRegChecked(kRegMicrophoneStatus, &final_00);
    ESP_LOGI(TAG,
             "DIAG FINAL: 0x11=%u (err=%s/%d) 0x00=%u (err=%s/%d)",
             final_11, esp_err_to_name(err_rd_final_11), static_cast<int>(err_rd_final_11),
             final_00, esp_err_to_name(err_rd_final_00), static_cast<int>(err_rd_final_00));

    // Summary: initial, post-write 0x11/0x00, final, all relevant I2C return codes
    ESP_LOGI(TAG,
             "DIAG summary: init(0x11=%u,0x00=%u) post_wr(0x11=%u,0x00=%u) "
             "final(0x11=%u,0x00=%u) "
             "errs(rd_init_11=%s/%d,rd_init_00=%s/%d,wr_11=%s/%d,rd_post20_11=%s/%d,"
             "wr_00=%s/%d,rd_post20_00=%s/%d,rd_final_11=%s/%d,rd_final_00=%s/%d)",
             init_11, init_00, post20_11, post20_00, final_11, final_00,
             esp_err_to_name(err_rd_init_11), static_cast<int>(err_rd_init_11),
             esp_err_to_name(err_rd_init_00), static_cast<int>(err_rd_init_00),
             esp_err_to_name(err_wr_11), static_cast<int>(err_wr_11),
             esp_err_to_name(err_rd_post20_11), static_cast<int>(err_rd_post20_11),
             esp_err_to_name(err_wr_00), static_cast<int>(err_wr_00),
             esp_err_to_name(err_rd_post20_00), static_cast<int>(err_rd_post20_00),
             esp_err_to_name(err_rd_final_11), static_cast<int>(err_rd_final_11),
             esp_err_to_name(err_rd_final_00), static_cast<int>(err_rd_final_00));

    // Gate only on 0x00 OPEN (and its write/read). Sticky 0x11 does NOT fail the route.
    if (err_wr_00 != ESP_OK || err_rd_final_00 != ESP_OK || final_00 != kAudioMicOpen) {
        ESP_LOGE(TAG,
                 "LINE/MIC route FAILED: 0x00 OPEN required (wr_00=%s/%d "
                 "rd_final_00=%s/%d final 0x00=%u want 1); final 0x11=%u (not gated)",
                 esp_err_to_name(err_wr_00), static_cast<int>(err_wr_00),
                 esp_err_to_name(err_rd_final_00), static_cast<int>(err_rd_final_00),
                 final_00, final_11);
        return false;
    }
    if (final_11 != kAudioMicClose) {
        ESP_LOGW(TAG,
                 "HEADPHONE_MIC_STATUS anomaly: 0x11=%u after CLOSE write "
                 "(want 0); continuing without gating mic path",
                 final_11);
    }

    ESP_LOGI(TAG,
             "LINE/MIC route enabled: STM32 0x11=%u (headset mic CLOSE attempt), "
             "0x00=%u (LINE/MIC OPEN)",
             final_11, final_00);
    return true;
}
