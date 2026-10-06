#ifndef CUBE_INTERNAL_AUDIO_CODEC_H_
#define CUBE_INTERNAL_AUDIO_CODEC_H_

#include "audio_codec.h"

#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>

// Cube V1.0 Variant B — built-in ES7210 mic + compile-safe speaker stub.
// Dedicated speaker attachment GPIOs are intentionally GPIO_NUM_NC until
// hardware validation. Do not claim AW88298 as the production speaker.
class CubeInternalAudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* in_ctrl_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;

    esp_codec_dev_handle_t input_dev_ = nullptr;

    void CreateMicChannel(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t din);

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    CubeInternalAudioCodec(void* i2c_master_handle, int input_sample_rate, int output_sample_rate,
                           gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t din,
                           uint8_t es7210_addr, bool input_reference);
    virtual ~CubeInternalAudioCodec();

    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif  // CUBE_INTERNAL_AUDIO_CODEC_H_
