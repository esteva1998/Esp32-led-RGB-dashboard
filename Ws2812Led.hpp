#pragma once
#include <cstdint>
#include <array>
#include "driver/rmt_tx.h"

// Controla un LED WS2812 mediante el periférico RMT nativo de ESP-IDF.
// RAII: el canal RMT se crea en el constructor y se libera en el destructor.
class Ws2812Led {
public:
    explicit Ws2812Led(gpio_num_t gpio);
    ~Ws2812Led();

    Ws2812Led(const Ws2812Led &) = delete;
    Ws2812Led &operator=(const Ws2812Led &) = delete;

    void setColor(uint8_t r, uint8_t g, uint8_t b);
    void off();

    std::array<uint8_t, 3> currentColor() const { return current_; }

private:
    rmt_channel_handle_t chan_ = nullptr;
    rmt_encoder_handle_t encoder_ = nullptr;
    std::array<uint8_t, 3> current_{0, 0, 0}; // orden RGB (se reordena a GRB al transmitir)
    uint8_t grb_buf_[3] = {0, 0, 0};
};
