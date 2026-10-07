#include "m144_linemic_route.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>

#include "i2c_device.h"

#define TAG "M144LineMic"

// From m5stack/M5Module-Audio src/M5Module_Audio.h
static constexpr uint8_t kM144Stm32Addr = 0x33;          // I2C_ADDR
static constexpr uint8_t kRegMicrophoneStatus = 0x00;    // MICROPHONE_STATUS (LINE/MIC jack)
static constexpr uint8_t kRegHeadphoneMicStatus = 0x11;  // HEADPHONE_MIC_STATUS
static constexpr uint8_t kAudioMicClose = 0;             // AUDIO_MIC_CLOSE
static constexpr uint8_t kAudioMicOpen = 1;              // AUDIO_MIC_OPEN

namespace {

class M144Stm32Device : public I2cDevice {
public:
    explicit M144Stm32Device(i2c_master_bus_handle_t i2c_bus) : I2cDevice(i2c_bus, kM144Stm32Addr) {}

    using I2cDevice::WriteReg;
    using I2cDevice::ReadReg;
};

}  // namespace

bool M144ProbeController(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "M144 controller probe failed: null I2C bus");
        return false;
    }
    esp_err_t err = i2c_master_probe(i2c_bus, kM144Stm32Addr, pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "M144 controller NOT found at I2C 0x%02X (%s)", kM144Stm32Addr,
                 esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "M144 controller found at I2C 0x%02X", kM144Stm32Addr);
    return true;
}

bool M144EnableLineMicPath(i2c_master_bus_handle_t i2c_bus) {
    if (!M144ProbeController(i2c_bus)) {
        ESP_LOGE(TAG, "LINE/MIC route FAILED: STM32 controller missing");
        return false;
    }

    M144Stm32Device stm32(i2c_bus);

    // Keep MICROPHONE_STATUS OPEN (0x00=1). Still attempt HEADPHONE_MIC CLOSE
    // (0x11=0) and log sticky readback, but do not gate the audio path on 0x11.
    stm32.WriteReg(kRegHeadphoneMicStatus, kAudioMicClose);
    stm32.WriteReg(kRegMicrophoneStatus, kAudioMicOpen);

    const uint8_t hp_mic = stm32.ReadReg(kRegHeadphoneMicStatus);
    const uint8_t line_mic = stm32.ReadReg(kRegMicrophoneStatus);
    if (line_mic != kAudioMicOpen) {
        ESP_LOGE(TAG,
                 "LINE/MIC route FAILED: readback line_mic(0x00)=%u (want 1); "
                 "hp_mic(0x11)=%u",
                 line_mic, hp_mic);
        return false;
    }
    if (hp_mic != kAudioMicClose) {
        ESP_LOGW(TAG,
                 "HEADPHONE_MIC_STATUS anomaly: 0x11=%u after CLOSE write "
                 "(want 0); continuing without gating mic path",
                 hp_mic);
    }

    ESP_LOGI(TAG,
             "LINE/MIC route enabled: STM32 0x11=%u (headset mic CLOSE attempt), "
             "0x00=%u (LINE/MIC OPEN)",
             hp_mic, line_mic);
    return true;
}
