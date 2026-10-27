#include "no_audio_codec.h"
#include "settings.h"

#include <esp_log.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#define TAG "NoAudioCodec"

NoAudioCodec::~NoAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

NoAudioCodecDuplex::NoAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}


NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

NoAudioCodecSimplex::NoAudioCodecSimplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din, i2s_std_slot_mask_t mic_slot_mask){
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = (spk_slot_mask == I2S_STD_SLOT_BOTH) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO,
            .slot_mask = (spk_slot_mask == I2S_STD_SLOT_BOTH) ? I2S_STD_SLOT_BOTH : ((spk_slot_mask == I2S_STD_SLOT_RIGHT) ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT),
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    spk_is_stereo_ = (spk_slot_mask == I2S_STD_SLOT_BOTH);
    if (spk_is_stereo_) {
        ESP_LOGI(TAG, "Simplex SPK configured for DUAL/STEREO output (Left + Right)");
    }

    // Create a new channel for MIC
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    if (mic_slot_mask == I2S_STD_SLOT_BOTH) {
        mic_is_stereo_ = true;
        std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
        std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
        gpio_pullup_dis(mic_din);
        gpio_pulldown_en(mic_din);
        ESP_LOGI(TAG, "Simplex MIC configured for DUAL/STEREO capture with auto-sensing (1-mic or 2-mic)");
    } else {
        mic_is_stereo_ = false;
        std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        std_cfg.slot_cfg.slot_mask = mic_slot_mask;
    }
    std_cfg.gpio_cfg.bclk = mic_sck;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_enabled_ || tx_handle_ == nullptr || samples <= 0) {
        return 0;
    }
    size_t target_size = spk_is_stereo_ ? (samples * 2) : samples;
    if (tx_buffer_.size() < target_size) {
        tx_buffer_.resize(target_size);
    }

    // output_volume_: 0-100
    // volume_factor_: 0-65536
    double vol = double(output_volume_) / 100.0;
    int32_t volume_factor = static_cast<int32_t>(vol * vol * 65536.0);
    if (spk_is_stereo_) {
        for (int i = 0; i < samples; i++) {
            int64_t temp = int64_t(data[i]) * volume_factor;
            int32_t sample_val;
            if (temp > INT32_MAX) {
                sample_val = INT32_MAX;
            } else if (temp < INT32_MIN) {
                sample_val = INT32_MIN;
            } else {
                sample_val = static_cast<int32_t>(temp);
            }
            tx_buffer_[2 * i] = sample_val;
            tx_buffer_[2 * i + 1] = sample_val;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_handle_, tx_buffer_.data(), samples * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            return 0;
        }
        return bytes_written / (2 * sizeof(int32_t));
    } else {
        for (int i = 0; i < samples; i++) {
            int64_t temp = int64_t(data[i]) * volume_factor;
            if (temp > INT32_MAX) {
                tx_buffer_[i] = INT32_MAX;
            } else if (temp < INT32_MIN) {
                tx_buffer_[i] = INT32_MIN;
            } else {
                tx_buffer_[i] = static_cast<int32_t>(temp);
            }
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_handle_, tx_buffer_.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            return 0;
        }
        return bytes_written / sizeof(int32_t);
    }
}

int NoAudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || rx_handle_ == nullptr || samples <= 0) {
        return 0;
    }
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    if (mic_is_stereo_) {
        if (rx_buffer_.size() < (size_t)(samples * 2)) {
            rx_buffer_.resize(samples * 2);
        }
        if (i2s_channel_read(rx_handle_, rx_buffer_.data(), samples * 2 * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
            return 0;
        }

        int pairs = bytes_read / (2 * sizeof(int32_t));
        if (pairs <= 0) {
            return 0;
        }

        // Auto-detect keberadaan mic Kiri dan Kanan pada ~30 frame pertama
        if (detect_frames_ > 0) {
            detect_frames_--;
            int64_t raw_l = 0, raw_r = 0;
            for (int i = 0; i < pairs; i++) {
                raw_l += (rx_buffer_[2 * i] >> mic_shift_);
                raw_r += (rx_buffer_[2 * i + 1] >> mic_shift_);
            }
            int32_t mean_l = raw_l / pairs;
            int32_t mean_r = raw_r / pairs;

            int32_t peak_ac_l = 0, peak_ac_r = 0;
            for (int i = 0; i < pairs; i++) {
                int32_t ac_l = std::abs((rx_buffer_[2 * i] >> mic_shift_) - mean_l);
                int32_t ac_r = std::abs((rx_buffer_[2 * i + 1] >> mic_shift_) - mean_r);
                if (ac_l > peak_ac_l) peak_ac_l = ac_l;
                if (ac_r > peak_ac_r) peak_ac_r = ac_r;
            }
            if (peak_ac_l > 350) {
                has_left_mic_ = true;
            }
            if (peak_ac_r > 350) {
                has_right_mic_ = true;
            }
            if (detect_frames_ == 0) {
                if (!has_left_mic_ && !has_right_mic_) {
                    has_left_mic_ = true;
                }
                ESP_LOGI(TAG, "Mic Auto-Detection: Left=%s, Right=%s -> Hardware: %s, Selected Mode: %s",
                         has_left_mic_ ? "Detected" : "None",
                         has_right_mic_ ? "Detected" : "None",
                         (has_left_mic_ && has_right_mic_) ? "Dual Mic" : "Single Mic",
                         mic_mode_.c_str());
            }
        }

        for (int i = 0; i < pairs; i++) {
            int32_t l = rx_buffer_[2 * i] >> mic_shift_;
            int32_t r = rx_buffer_[2 * i + 1] >> mic_shift_;
            int32_t value;

            if (mic_mode_ == "blend") {
                if (has_left_mic_ && has_right_mic_) {
                    value = (l + r) / 2;
                } else if (has_right_mic_) {
                    value = r;
                } else {
                    value = l;
                }
            } else if (mic_mode_ == "right") {
                value = r;
            } else {
                // "primary" / "left" (Default):
                // Menggunakan mikrofon tunggal bersih tanpa cacat fase (comb-filtering)
                // Menghilangkan suara cempreng/hampa di kaleng sehingga akurasi AI maksimal!
                if (has_left_mic_) {
                    value = l;
                } else if (has_right_mic_) {
                    value = r;
                } else {
                    value = l;
                }
            }
            dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
        }
        return pairs;
    } else {
        if (rx_buffer_.size() < (size_t)samples) {
            rx_buffer_.resize(samples);
        }
        if (i2s_channel_read(rx_handle_, rx_buffer_.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
            return 0;
        }

        samples = bytes_read / sizeof(int32_t);
        for (int i = 0; i < samples; i++) {
            int32_t value = rx_buffer_[i] >> mic_shift_;
            dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
        }
        return samples;
    }
}

int NoAudioCodec::ReadStereo(int16_t* left_dest, int16_t* right_dest, int samples) {
    if (!input_enabled_ || rx_handle_ == nullptr || samples <= 0) {
        return 0;
    }
    size_t bytes_read = 0;
    constexpr uint32_t kReadTimeoutMs = 100;

    if (mic_is_stereo_) {
        if (rx_buffer_.size() < (size_t)(samples * 2)) {
            rx_buffer_.resize(samples * 2);
        }
        if (i2s_channel_read(rx_handle_, rx_buffer_.data(), samples * 2 * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
            return 0;
        }

        int pairs = bytes_read / (2 * sizeof(int32_t));
        for (int i = 0; i < pairs; i++) {
            int32_t l = rx_buffer_[2 * i] >> mic_shift_;
            int32_t r = rx_buffer_[2 * i + 1] >> mic_shift_;
            if (left_dest) {
                left_dest[i] = (l > INT16_MAX) ? INT16_MAX : (l < -INT16_MAX) ? -INT16_MAX : (int16_t)l;
            }
            if (right_dest) {
                right_dest[i] = (r > INT16_MAX) ? INT16_MAX : (r < -INT16_MAX) ? -INT16_MAX : (int16_t)r;
            }
        }
        return pairs;
    } else {
        if (rx_buffer_.size() < (size_t)samples) {
            rx_buffer_.resize(samples);
        }
        if (i2s_channel_read(rx_handle_, rx_buffer_.data(), samples * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
            return 0;
        }
        int s = bytes_read / sizeof(int32_t);
        for (int i = 0; i < s; i++) {
            int32_t val = rx_buffer_[i] >> mic_shift_;
            int16_t v = (val > INT16_MAX) ? INT16_MAX : (val < -INT16_MAX) ? -INT16_MAX : (int16_t)val;
            if (left_dest) left_dest[i] = v;
            if (right_dest) right_dest[i] = 0;
        }
        return s;
    }
}

void NoAudioCodec::Start() {
    AudioCodec::Start();
    Settings settings("audio", false);
    mic_shift_ = settings.GetInt("mic_shift", 11);
    if (mic_shift_ < 0 || mic_shift_ > 24) {
        mic_shift_ = 11;
    }
    mic_mode_ = settings.GetString("mic_mode", "primary");
    has_left_mic_ = false;
    has_right_mic_ = false;
    detect_frames_ = 30;
    ESP_LOGI(TAG, "NoAudioCodec started, mic_shift=%d (+6dB), mic_stereo=%d, mic_mode=%s", mic_shift_, mic_is_stereo_, mic_mode_.c_str());
}

void NoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void NoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

// Delegating constructor: calls the main constructor with default slot mask
NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_din) 
    : NoAudioCodecSimplexPdm(input_sample_rate, output_sample_rate, spk_bclk, spk_ws, spk_dout, I2S_STD_SLOT_LEFT, mic_sck, mic_din) {
    // All initialization is handled by the delegated constructor
}

NoAudioCodecSimplexPdm::NoAudioCodecSimplexPdm(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, i2s_std_slot_mask_t spk_slot_mask, gpio_num_t mic_sck, gpio_num_t mic_din) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    // Create a new channel for speaker
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    tx_chan_cfg.intr_priority = 0;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, NULL));


    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = spk_slot_mask,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));
#if SOC_I2S_SUPPORTS_PDM_RX
    // Create a new channel for MIC in PDM mode
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle_));
    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        /* The data bit-width of PDM mode is fixed to 16 */
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = mic_sck,
            .din = mic_din,

            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));
#else
    ESP_LOGE(TAG, "PDM is not supported");
#endif
    ESP_LOGI(TAG, "Simplex channels created");
}

int NoAudioCodecSimplexPdm::Read(int16_t* dest, int samples) {
    size_t bytes_read;

    // PDM 解调后的数据位宽为 16 位，直接读取到目标缓冲区
    if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t), &bytes_read, portMAX_DELAY) != ESP_OK) {
        ESP_LOGE(TAG, "Read Failed!");
        return 0;
    }

    samples = bytes_read / sizeof(int16_t);
    if (input_gain_ > 0) {
        int gain_factor = (int)input_gain_;
        for (int i = 0; i < samples; i++) {
            int32_t amplified = dest[i] * gain_factor;
            dest[i] = (amplified > INT16_MAX) ? INT16_MAX : (amplified < -INT16_MAX) ? -INT16_MAX : (int16_t)amplified;
        }
    }
    return samples;
}
