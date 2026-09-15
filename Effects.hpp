#pragma once
#include "Ws2812Led.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum class EffectMode : uint8_t {
    None = 0,
    Rainbow = 1,
    Breathing = 2,
    Strobe = 3,
};

// Corre efectos en una tarea de FreeRTOS aparte, sin bloquear el servidor HTTP.
// Al activar un efecto, este toma control del LED hasta que se llama a stop()
// (que ocurre automáticamente si el usuario mueve un slider o toca un color).
class EffectsManager {
public:
    explicit EffectsManager(Ws2812Led &led);
    ~EffectsManager();

    void start(EffectMode mode);
    void stop();
    EffectMode current() const { return mode_; }

private:
    static void taskTrampoline(void *arg);
    void taskLoop();

    Ws2812Led &led_;
    TaskHandle_t task_ = nullptr;
    volatile EffectMode mode_ = EffectMode::None;
};
