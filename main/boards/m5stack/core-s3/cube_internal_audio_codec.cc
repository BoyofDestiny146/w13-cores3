#include "cube_internal_audio_codec.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/i2s_tdm.h>

#define TAG "CubeInternalAudio"

CubeInternalAudioCodec::CubeInternalAudioCodec(void* i2c_master_handle, int input_sample_rate,
                                               int output_sample_rate, gpio_num_t mclk,
                                               gpio_num_t bclk, gpio_num_t ws, gpio_num_t din,
                                               uint8_t es7210_addr, bool input_reference) {
    duplex_ = true;
    input_reference_ = input_reference;
    input_channels_ = input_reference_ ? 2 : 1;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_gain_ = 30;

    CreateMicChannel(mclk, bclk, ws, din);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = nullptr,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = (i2c_port_t)1,
        .addr = es7210_addr,
        .bus_handle = i2c_master_handle,
    };
    in_ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(in_ctrl_if_ != NULL);

    es7210_codec_cfg_t es7210_cfg = {};
    es7210_cfg.ctrl_if = in_ctrl_if_;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
    in_codec_if_ = es7210_codec_new(&es7210_cfg);
    assert(in_codec_if_ != NULL);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = in_codec_if_,
        .data_if = data_if_,
    };
    input_dev_ = esp_codec_dev_new(&dev_cfg);
    assert(input_dev_ != NULL);

    ESP_LOGW(TAG,
             "Variant B: ES7210 mic ready on I2S_NUM_0; speaker is a compile-safe stub "
             "(dedicated attachment pins unvalidated — not M144 Config B, not AW88298 production)");
}

CubeInternalAudioCodec::~CubeInternalAudioCodec() {
    if (input_dev_ != nullptr) {
        esp_codec_dev_close(input_dev_);
        esp_codec_dev_delete(input_dev_);
    }
    if (in_codec_if_ != nullptr) {
        audio_codec_delete_codec_if(in_codec_if_);
    }
    if (in_ctrl_if_ != nullptr) {
        audio_codec_delete_ctrl_if(in_ctrl_if_);
    }
    if (data_if_ != nullptr) {
        audio_codec_delete_data_if(data_if_);
    }
    if (rx_handle_ != nullptr) {
        i2s_channel_disable(rx_handle_);
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
}

void CubeInternalAudioCodec::CreateMicChannel(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                              gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    ESP_LOGI(TAG, "ES7210 IOs: mclk=%d bclk=%d ws=%d din=%d (I2S_NUM_0 RX-only)", mclk, bclk, ws,
             din);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    // RX-only: do not claim TX / speaker GPIOs (dedicated attachment TBD).
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = (uint32_t)input_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 |
                                                I2S_TDM_SLOT3),
                .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
        .gpio_cfg =
            {
                .mclk = mclk,
                .bclk = bclk,
                .ws = ws,
                .dout = I2S_GPIO_UNUSED,
                .din = din,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
}

void CubeInternalAudioCodec::SetOutputVolume(int volume) {
    // Speaker stub: volume is tracked for UI/Nexus but has no amp behind it yet.
    AudioCodec::SetOutputVolume(volume);
}

void CubeInternalAudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 2,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
            .sample_rate = (uint32_t)output_sample_rate_,
            .mclk_multiple = 0,
        };
        if (input_reference_) {
            fs.channel_mask |= ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        }
        ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_, &fs));
        ESP_ERROR_CHECK(
            esp_codec_dev_set_in_channel_gain(input_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
                                             input_gain_));
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_close(input_dev_));
    }
    AudioCodec::EnableInput(enable);
}

void CubeInternalAudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_LOGW(TAG, "Speaker stub enabled — no dedicated attachment GPIOs validated yet");
    }
    AudioCodec::EnableOutput(enable);
}

int CubeInternalAudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_read(input_dev_, (void*)dest, samples * sizeof(int16_t)));
    }
    return samples;
}

int CubeInternalAudioCodec::Write(const int16_t* data, int samples) {
    (void)data;
    // Discard PCM until dedicated speaker attachment pins are validated.
    return samples;
}
