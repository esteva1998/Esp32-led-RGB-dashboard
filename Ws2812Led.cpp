#include "Ws2812Led.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

namespace {

// Encoder combinado: bytes de color con timings WS2812 + pulso de reset.
struct Ws2812Encoder {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
};

size_t encodeFn(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                 const void *primary_data, size_t data_size,
                 rmt_encode_state_t *ret_state) {
    auto *enc = __containerof(encoder, Ws2812Encoder, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = (rmt_encode_state_t)0;
    size_t encoded = 0;

    if (enc->state == 0) {
        encoded += enc->bytes_encoder->encode(enc->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) {
            *ret_state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
            return encoded;
        }
    }
    if (enc->state == 1) {
        encoded += enc->copy_encoder->encode(enc->copy_encoder, channel, &enc->reset_code, sizeof(enc->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            enc->state = 0;
            state = (rmt_encode_state_t)(state | RMT_ENCODING_COMPLETE);
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state = (rmt_encode_state_t)(state | RMT_ENCODING_MEM_FULL);
        }
    }
    *ret_state = state;
    return encoded;
}

esp_err_t resetFn(rmt_encoder_t *encoder) {
    auto *enc = __containerof(encoder, Ws2812Encoder, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = 0;
    return ESP_OK;
}

esp_err_t delFn(rmt_encoder_t *encoder) {
    auto *enc = __containerof(encoder, Ws2812Encoder, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    delete enc;
    return ESP_OK;
}

rmt_encoder_handle_t createEncoder() {
    auto *enc = new Ws2812Encoder{};
    enc->base.encode = encodeFn;
    enc->base.del = delFn;
    enc->base.reset = resetFn;

    rmt_bytes_encoder_config_t bytes_cfg = {};
    bytes_cfg.bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 };
    bytes_cfg.bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 3, .level1 = 0 };
    bytes_cfg.flags.msb_first = 1;
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_cfg, &enc->bytes_encoder));

    rmt_copy_encoder_config_t copy_cfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_cfg, &enc->copy_encoder));

    enc->reset_code = { .duration0 = 250, .level0 = 0, .duration1 = 250, .level1 = 0 };

    return &enc->base;
}

}  // namespace

Ws2812Led::Ws2812Led(gpio_num_t gpio) {
    rmt_tx_channel_config_t tx_cfg = {};
    tx_cfg.gpio_num = gpio;
    tx_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_cfg.resolution_hz = 10 * 1000 * 1000;
    tx_cfg.mem_block_symbols = 64;
    tx_cfg.trans_queue_depth = 4;

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &chan_));
    encoder_ = createEncoder();
    ESP_ERROR_CHECK(rmt_enable(chan_));
}

Ws2812Led::~Ws2812Led() {
    if (chan_) {
        rmt_disable(chan_);
        rmt_del_channel(chan_);
    }
    if (encoder_) {
        rmt_del_encoder(encoder_);
    }
}

void Ws2812Led::setColor(uint8_t r, uint8_t g, uint8_t b) {
    current_ = {r, g, b};
    grb_buf_[0] = g;
    grb_buf_[1] = r;
    grb_buf_[2] = b;

    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;

    ESP_ERROR_CHECK(rmt_transmit(chan_, encoder_, grb_buf_, sizeof(grb_buf_), &tx_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(chan_, portMAX_DELAY));
}

void Ws2812Led::off() { setColor(0, 0, 0); }
