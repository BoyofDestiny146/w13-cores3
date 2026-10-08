#include "es8388_audio_codec.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <climits>
#include <cstdint>

#define TAG "Es8388AudioCodec"

Es8388AudioCodec::Es8388AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
                                   int input_sample_rate, int output_sample_rate, gpio_num_t mclk,
                                   gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                   gpio_num_t pa_pin, uint8_t es8388_addr, bool input_reference) {
    duplex_ = true;                              // 是否双工
    input_reference_ = input_reference;          // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1;  // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 24;

    pa_pin_ = pa_pin;
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8388_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8388_codec_cfg_t es8388_cfg = {};
    es8388_cfg.ctrl_if = ctrl_if_;
    es8388_cfg.gpio_if = gpio_if_;
    es8388_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    // M144 EXTERNAL: ES8388 follows ESP32 I2S clocks (official Module-Audio).
    es8388_cfg.master_mode = false;
#else
    es8388_cfg.master_mode = true;
#endif
    es8388_cfg.pa_pin = pa_pin;
    es8388_cfg.pa_reverted = false;
    es8388_cfg.hw_gain.pa_voltage = 5.0;
    es8388_cfg.hw_gain.codec_dac_voltage = 3.3;
    codec_if_ = es8388_codec_new(&es8388_cfg);
    assert(codec_if_ != NULL);

    esp_codec_dev_cfg_t outdev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    output_dev_ = esp_codec_dev_new(&outdev_cfg);
    assert(output_dev_ != NULL);

    esp_codec_dev_cfg_t indev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&indev_cfg);
    assert(input_dev_ != NULL);
    esp_codec_set_disable_when_closed(output_dev_, false);
    esp_codec_set_disable_when_closed(input_dev_, false);
    ESP_LOGI(TAG, "Es8388AudioCodec initialized");
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    ESP_LOGI(TAG, "M144 ES8388 clock role: codec SLAVE, ESP32 I2S MASTER");
#endif
}

Es8388AudioCodec::~Es8388AudioCodec() {
    ESP_ERROR_CHECK(esp_codec_dev_close(output_dev_));
    esp_codec_dev_delete(output_dev_);
    ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    esp_codec_dev_delete(input_dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8388AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                            gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {.sample_rate_hz = (uint32_t)output_sample_rate_,
                    .clk_src = I2S_CLK_SRC_DEFAULT,
                    .ext_clk_freq_hz = 0,
                    .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_STD_SLOT_BOTH,
                     .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .ws_pol = false,
                     .bit_shift = true,
                     .left_align = true,
                     .big_endian = false,
                     .bit_order_lsb = false},
        .gpio_cfg = {.mclk = mclk,
                     .bclk = bclk,
                     .ws = ws,
                     .dout = dout,
                     .din = din,
                     .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8388AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

bool Es8388AudioCodec::SelectMicInputLin1() {
    if (ctrl_if_ == nullptr) {
        ESP_LOGE(TAG, "SelectMicInputLin1: ctrl_if_ is null");
        return false;
    }

    // M5Module-Audio ES8388::setADCInput(ADC_INPUT_LINPUT1_RINPUT1):
    //   ADCCONTROL2 = (reg & 0x0f) | 0x00
    //   ADCCONTROL3 = 0x00
    uint8_t adc2 = 0;
    if (ctrl_if_->read_reg(ctrl_if_, 0x0a, 1, &adc2, 1) != 0) {
        ESP_LOGE(TAG, "SelectMicInputLin1: failed to read ADCCONTROL2");
        return false;
    }
    adc2 = static_cast<uint8_t>((adc2 & 0x0f) | 0x00);
    uint8_t adc3 = 0x00;
    if (ctrl_if_->write_reg(ctrl_if_, 0x0a, 1, &adc2, 1) != 0 ||
        ctrl_if_->write_reg(ctrl_if_, 0x0b, 1, &adc3, 1) != 0) {
        ESP_LOGE(TAG, "SelectMicInputLin1: failed to write ADCCONTROL2/3");
        return false;
    }

    uint8_t verify2 = 0;
    uint8_t verify3 = 0;
    uint8_t pga = 0;
    ctrl_if_->read_reg(ctrl_if_, 0x0a, 1, &verify2, 1);
    ctrl_if_->read_reg(ctrl_if_, 0x0b, 1, &verify3, 1);
    ctrl_if_->read_reg(ctrl_if_, 0x09, 1, &pga, 1);
    ESP_LOGI(TAG,
             "ES8388 microphone input selected: LIN1/RIN1 "
             "(ADCCONTROL2=0x%02X ADCCONTROL3=0x%02X PGA/ADCCONTROL1=0x%02X unchanged)",
             verify2, verify3, pga);
    return true;
}

bool Es8388AudioCodec::SelectMicInputLin2() {
    if (ctrl_if_ == nullptr) {
        ESP_LOGE(TAG, "SelectMicInputLin2: ctrl_if_ is null");
        return false;
    }

    // M5Module-Audio ES8388::setADCInput(ADC_INPUT_LINPUT2_RINPUT2):
    //   ADCCONTROL2 = (reg & 0x0f) | 0x10
    //   ADCCONTROL3 = 0x80
    uint8_t adc2 = 0;
    if (ctrl_if_->read_reg(ctrl_if_, 0x0a, 1, &adc2, 1) != 0) {
        ESP_LOGE(TAG, "SelectMicInputLin2: failed to read ADCCONTROL2");
        return false;
    }
    adc2 = static_cast<uint8_t>((adc2 & 0x0f) | 0x10);
    uint8_t adc3 = 0x80;
    if (ctrl_if_->write_reg(ctrl_if_, 0x0a, 1, &adc2, 1) != 0 ||
        ctrl_if_->write_reg(ctrl_if_, 0x0b, 1, &adc3, 1) != 0) {
        ESP_LOGE(TAG, "SelectMicInputLin2: failed to write ADCCONTROL2/3");
        return false;
    }

    uint8_t verify2 = 0;
    uint8_t verify3 = 0;
    uint8_t pga = 0;
    ctrl_if_->read_reg(ctrl_if_, 0x0a, 1, &verify2, 1);
    ctrl_if_->read_reg(ctrl_if_, 0x0b, 1, &verify3, 1);
    ctrl_if_->read_reg(ctrl_if_, 0x09, 1, &pga, 1);
    ESP_LOGI(TAG,
             "M144 external microphone ADC selected: LIN2/RIN2 "
             "(ADCCONTROL2=0x%02X ADCCONTROL3=0x%02X PGA/ADCCONTROL1=0x%02X unchanged)",
             verify2, verify3, pga);

#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    // Official M5Module-Audio setADCInput(LIN2) wants ADCCONTROL3 exactly 0x80.
    // esp_codec_dev open leaves bit1 (0x02) → prior bench readback 0x82; force exact.
    adc3 = 0x80;
    if (ctrl_if_->write_reg(ctrl_if_, 0x0b, 1, &adc3, 1) != 0) {
        ESP_LOGE(TAG, "SelectMicInputLin2: failed to force ADCCONTROL3=0x80");
        return false;
    }
    verify2 = 0;
    verify3 = 0;
    ctrl_if_->read_reg(ctrl_if_, 0x0a, 1, &verify2, 1);
    ctrl_if_->read_reg(ctrl_if_, 0x0b, 1, &verify3, 1);
    ESP_LOGI(TAG, "M144 LIN2 ADC exact official config: ADCCONTROL2=0x%02X ADCCONTROL3=0x%02X",
             verify2, verify3);
#endif
    return true;
}

void Es8388AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // DuplexStereoSlots: keep ESP I2S RX Philips stereo (match ES8388 / M5).
        // AFE still sees mono via left-channel select in Read().
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 2,
            .channel_mask = static_cast<uint16_t>(ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                  ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)),
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
#else
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = (uint8_t)input_channels_,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
#endif
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        ESP_LOGI(TAG,
                 "M144 DuplexStereoSlots: RX stereo "
                 "(open channel=%u channel_mask=0x%x bits=16 rate=%u Hz Philips)",
                 static_cast<unsigned>(fs.channel), static_cast<unsigned>(fs.channel_mask),
                 static_cast<unsigned>(fs.sample_rate));
        // Everest ADCCONTROL3 DS=1 (LIN2/RIN2) feeds the left ADC; M5 LIN2 path
        // uses that. Prefer left for AFE after stereo RX.
        ESP_LOGI(TAG, "M144 DuplexStereoSlots: AFE source channel=left");
#endif
        if (input_reference_) {
            uint8_t gain = (11 << 4) + 0;
            ctrl_if_->write_reg(ctrl_if_, 0x09, 1, &gain, 1);
        } else {
            ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(input_dev_, input_gain_));
        }
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void Es8388AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        if (!output_device_opened_) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
            // DuplexStereoSlots: TX Philips stereo; mono app/TTS duplicated in Write().
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 2,
                .channel_mask = static_cast<uint16_t>(ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                      ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)),
                .sample_rate = (uint32_t)output_sample_rate_,
                .mclk_multiple = 0,
            };
#else
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = (uint32_t)output_sample_rate_,
                .mclk_multiple = 0,
            };
#endif
            ESP_ERROR_CHECK(esp_codec_dev_open(output_dev_, &fs));
            output_device_opened_ = true;
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
            ESP_LOGI(TAG,
                     "M144 DuplexStereoSlots: TX stereo "
                     "(open channel=%u channel_mask=0x%x bits=16 rate=%u Hz Philips)",
                     static_cast<unsigned>(fs.channel), static_cast<unsigned>(fs.channel_mask),
                     static_cast<unsigned>(fs.sample_rate));
#endif
        }
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(output_dev_, output_volume_));

        // Set analog output volume to 0dB, default is -45dB
        uint8_t reg_val = 30;               // 0dB
        uint8_t regs[] = {46, 47, 48, 49};  // HP_LVOL, HP_RVOL, SPK_LVOL, SPK_RVOL
        for (uint8_t reg : regs) {
            ctrl_if_->write_reg(ctrl_if_, reg, 1, &reg_val, 1);
        }

        ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(output_dev_, false));
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 1);
        }
    } else {
        // Keep TX open because the paired RX channel may use it as the clock
        // source. Reopening the shared duplex interface after WiFi provisioning
        // can otherwise leave playback silent on ESP-IDF 6.
        if (output_device_opened_) {
            ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(output_dev_, true));
        }
        if (pa_pin_ != GPIO_NUM_NC) {
            gpio_set_level(pa_pin_, 0);
        }
    }
    AudioCodec::EnableOutput(enable);
}

int Es8388AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // Stereo I2S RX → keep left (LIN2 mic) → mono buffer for AFE/resampler.
        if (dest != nullptr && samples > 0) {
            duplex_stereo_rx_.resize(static_cast<size_t>(samples) * 2);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_read(
                input_dev_, duplex_stereo_rx_.data(), duplex_stereo_rx_.size() * sizeof(int16_t)));
            for (int i = 0; i < samples; ++i) {
                dest[i] = duplex_stereo_rx_[static_cast<size_t>(i) * 2];  // left
            }
        }
        // M144PcmDiag: measure post stereo→mono select (AFE-facing PCM).
        if (dest != nullptr && samples > 0) {
            static TickType_t win_start = 0;
            static int64_t win_n = 0;
            static int16_t win_min = INT16_MAX;
            static int16_t win_max = INT16_MIN;
            static int32_t win_peak = 0;
            static int64_t win_sum_abs = 0;

            if (win_start == 0) {
                win_start = xTaskGetTickCount();
            }
            for (int i = 0; i < samples; ++i) {
                const int16_t s = dest[i];
                if (s < win_min) {
                    win_min = s;
                }
                if (s > win_max) {
                    win_max = s;
                }
                const int32_t a = (s < 0) ? -static_cast<int32_t>(s) : static_cast<int32_t>(s);
                if (a > win_peak) {
                    win_peak = a;
                }
                win_sum_abs += a;
            }
            win_n += samples;

            const TickType_t now = xTaskGetTickCount();
            if ((now - win_start) >= pdMS_TO_TICKS(1000) && win_n > 0) {
                const int32_t mean_abs = static_cast<int32_t>(win_sum_abs / win_n);
                ESP_LOGI("M144PcmDiag",
                         "RAW PCM n=%lld min=%d max=%d peak_abs=%ld mean_abs=%ld "
                         "rate=%dHz ch=%d bits=16 signed (post L-select)",
                         static_cast<long long>(win_n), static_cast<int>(win_min),
                         static_cast<int>(win_max), static_cast<long>(win_peak),
                         static_cast<long>(mean_abs), input_sample_rate_, input_channels_);
                win_start = now;
                win_n = 0;
                win_min = INT16_MAX;
                win_max = INT16_MIN;
                win_peak = 0;
                win_sum_abs = 0;
            }
        }
#else
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
#endif  // CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    }
    return samples;
}

int Es8388AudioCodec::Write(const int16_t* data, int samples) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
    // TtsPathProbe: observe-only Write stats (no sample dump). Rate-limit first N + ~1s.
    {
        static uint32_t call_count = 0;
        static int64_t cum_samples = 0;
        static TickType_t last_log = 0;
        ++call_count;
        if (samples > 0) {
            cum_samples += samples;
        }
        const TickType_t now = xTaskGetTickCount();
        if (call_count <= 16 || last_log == 0 ||
            (now - last_log) >= pdMS_TO_TICKS(1000)) {
            const int bytes =
                samples > 0 ? samples * static_cast<int>(sizeof(int16_t)) : 0;
            ESP_LOGI("TtsPathProbe",
                     "Write calls=%lu samples=%d bytes=%d output_enabled_=%d "
                     "cum_samples=%lld",
                     static_cast<unsigned long>(call_count), samples, bytes,
                     output_enabled_ ? 1 : 0, static_cast<long long>(cum_samples));
            last_log = now;
        }
    }
#endif
    if (output_enabled_ && output_dev_ && data != nullptr) {
#if CONFIG_CUBE_AUDIO_VARIANT_EXTERNAL
        // Duplicate mono app/TTS into L and R for stereo Philips TX.
        if (samples > 0) {
            duplex_stereo_tx_.resize(static_cast<size_t>(samples) * 2);
            for (int i = 0; i < samples; ++i) {
                const int16_t s = data[i];
                duplex_stereo_tx_[static_cast<size_t>(i) * 2] = s;
                duplex_stereo_tx_[static_cast<size_t>(i) * 2 + 1] = s;
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(
                output_dev_, duplex_stereo_tx_.data(), duplex_stereo_tx_.size() * sizeof(int16_t)));
        }
#else
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_write(output_dev_, (void*)data, samples * sizeof(int16_t)));
#endif
    }
    return samples;
}
