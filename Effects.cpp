#include "Effects.hpp"
#include "esp_log.h"
#include <cmath>

namespace {

// Convierte HSV (0-360, 0-1, 0-1) a RGB (0-255) para el efecto arcoíris.
void hsvToRgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = v - c;
    float rp, gp, bp;
    if (h < 60)       { rp = c; gp = x; bp = 0; }
    else if (h < 120) { rp = x; gp = c; bp = 0; }
    else if (h < 180) { rp = 0; gp = c; bp = x; }
    else if (h < 240) { rp = 0; gp = x; bp = c; }
    else if (h < 300) { rp = x; gp = 0; bp = c; }
    else              { rp = c; gp = 0; bp = x; }
    r = (uint8_t)((rp + m) * 255);
    g = (uint8_t)((gp + m) * 255);
    b = (uint8_t)((bp + m) * 255);
}

}  // namespace

EffectsManager::EffectsManager(Ws2812Led &led) : led_(led) {
    xTaskCreate(taskTrampoline, "effects_task", 4096, this, 5, &task_);
}

EffectsManager::~EffectsManager() {
    if (task_) vTaskDelete(task_);
}

void EffectsManager::start(EffectMode mode) { mode_ = mode; }
void EffectsManager::stop() { mode_ = EffectMode::None; }

void EffectsManager::taskTrampoline(void *arg) {
    static_cast<EffectsManager *>(arg)->taskLoop();
}

void EffectsManager::taskLoop() {
    float hue = 0;
    float breath_phase = 0;
    bool strobe_on = false;

    while (true) {
        switch (mode_) {
            case EffectMode::Rainbow: {
                uint8_t r, g, b;
                hsvToRgb(hue, 1.0f, 1.0f, r, g, b);
                led_.setColor(r, g, b);
                hue += 2.0f;
                if (hue >= 360.0f) hue -= 360.0f;
                vTaskDelay(pdMS_TO_TICKS(30));
                break;
            }
            case EffectMode::Breathing: {
                float v = (sinf(breath_phase) + 1.0f) / 2.0f; // 0..1
                uint8_t r, g, b;
                hsvToRgb(200.0f, 0.6f, v, r, g, b); // tono celeste respirando
                led_.setColor(r, g, b);
                breath_phase += 0.05f;
                if (breath_phase > 2 * (float)M_PI) breath_phase -= 2 * (float)M_PI;
                vTaskDelay(pdMS_TO_TICKS(30));
                break;
            }
            case EffectMode::Strobe: {
                strobe_on = !strobe_on;
                if (strobe_on) led_.setColor(255, 255, 255);
                else led_.off();
                vTaskDelay(pdMS_TO_TICKS(80));
                break;
            }
            case EffectMode::None:
            default:
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
        }
    }
}
