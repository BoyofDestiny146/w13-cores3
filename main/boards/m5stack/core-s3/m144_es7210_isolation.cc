#include "m144_es7210_isolation.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "i2c_device.h"

#define TAG "M144ExtInit"

// Schematic / esp_codec_dev: ES7210 7-bit address is 0x40
// (ES7210_CODEC_DEFAULT_ADDR 0x80 is the 8-bit form used by esp_codec_dev).
static constexpr uint8_t kEs7210Addr7Bit = 0x40;
static constexpr uint8_t kRegSdpInterfaceConfig2 = 0x12;  // SDP INTERFACE CONFIG 2
static constexpr uint8_t kSpSdout1Tri = 0x10;             // bit4 SP_SDOUT1_TRI

namespace {

class Es7210IsolationDevice : public I2cDevice {
public:
    explicit Es7210IsolationDevice(i2c_master_bus_handle_t i2c_bus)
        : I2cDevice(i2c_bus, kEs7210Addr7Bit) {}

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

bool Es7210ForceSdout1HiZ(i2c_master_bus_handle_t i2c_bus) {
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "ES7210 Hi-Z FAILED: null I2C bus");
        return false;
    }

    esp_err_t probe = i2c_master_probe(i2c_bus, kEs7210Addr7Bit, pdMS_TO_TICKS(200));
    if (probe != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 Hi-Z FAILED: probe 0x%02X -> %s/%d", kEs7210Addr7Bit,
                 esp_err_to_name(probe), static_cast<int>(probe));
        return false;
    }

    Es7210IsolationDevice es7210(i2c_bus);

    uint8_t before = 0;
    esp_err_t err_rd = es7210.ReadRegChecked(kRegSdpInterfaceConfig2, &before);
    if (err_rd != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 Hi-Z FAILED: read 0x12 -> %s/%d", esp_err_to_name(err_rd),
                 static_cast<int>(err_rd));
        return false;
    }

    const uint8_t after_write = static_cast<uint8_t>(before | kSpSdout1Tri);
    esp_err_t err_wr = es7210.WriteRegChecked(kRegSdpInterfaceConfig2, after_write);
    ESP_LOGI(TAG, "ES7210 0x12 RMW: before=0x%02X write=0x%02X wr=%s/%d", before, after_write,
             esp_err_to_name(err_wr), static_cast<int>(err_wr));
    if (err_wr != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 Hi-Z FAILED: write 0x12");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t readback = 0;
    esp_err_t err_rb = es7210.ReadRegChecked(kRegSdpInterfaceConfig2, &readback);
    const bool bit4 = (readback & kSpSdout1Tri) != 0;
    ESP_LOGI(TAG, "ES7210 0x12 readback=0x%02X bit4(SP_SDOUT1_TRI)=%d rb=%s/%d", readback,
             bit4 ? 1 : 0, esp_err_to_name(err_rb), static_cast<int>(err_rb));

    if (err_rb != ESP_OK || !bit4) {
        ESP_LOGE(TAG,
                 "ES7210 Hi-Z NOT CONFIRMED — fail-safe: do not start EXTERNAL I2S RX on GPIO14");
        return false;
    }

    ESP_LOGI(TAG, "ES7210 SDOUT1 Hi-Z confirmed (0x12 bit4 set)");
    return true;
}
