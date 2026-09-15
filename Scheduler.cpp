#include "Scheduler.hpp"
#include "Storage.hpp"
#include "WebServer.hpp"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctime>
#include <cstdlib>

namespace scheduler {

static const char *TAG = "scheduler";
static bool s_time_synced = false;
static Ws2812Led *s_led = nullptr;
static EffectsManager *s_effects = nullptr;

void init_time() {
    setenv("TZ", "<-05>5", 1); // Peru, UTC-5, sin horario de verano
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        s_time_synced = true;
        ESP_LOGI(TAG, "Hora sincronizada por SNTP");
    } else {
        ESP_LOGW(TAG, "No se pudo sincronizar la hora todavia (se reintenta sola en segundo plano)");
    }
}

bool is_time_synced() { return s_time_synced; }

static void applyScheduleAction(const storage::Schedule &sched) {
    switch (sched.action) {
        case 0: // color fijo
            s_effects->stop();
            s_led->setColor(sched.r, sched.g, sched.b);
            break;
        case 1: // efecto
            s_effects->start(static_cast<EffectMode>(sched.effect));
            break;
        case 2: // escena guardada
            apply_scene_by_name(sched.scene_name);
            break;
    }
}

static void checkTaskFn(void *) {
    // Guarda el último minuto en que se disparó el INICIO y el FIN de cada
    // horario (por índice), para no repetir el disparo dentro del mismo minuto.
    int last_start_fired[storage::kMaxSchedules];
    int last_end_fired[storage::kMaxSchedules];
    for (size_t i = 0; i < storage::kMaxSchedules; i++) {
        last_start_fired[i] = -1;
        last_end_fired[i] = -1;
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));

        if (!s_time_synced) {
            if (esp_netif_sntp_sync_wait(0) == ESP_OK) s_time_synced = true;
            else continue;
        }

        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        int current_minute_of_day = timeinfo.tm_hour * 60 + timeinfo.tm_min;

        auto schedules = storage::load_schedules();
        for (size_t i = 0; i < schedules.size() && i < storage::kMaxSchedules; i++) {
            const auto &sched = schedules[i];
            if (!sched.enabled) continue;

            if (sched.start_hour == timeinfo.tm_hour && sched.start_minute == timeinfo.tm_min &&
                last_start_fired[i] != current_minute_of_day) {
                applyScheduleAction(sched);
                ESP_LOGI(TAG, "Horario #%d iniciado -> %02d:%02d", (int)i, sched.start_hour, sched.start_minute);
                last_start_fired[i] = current_minute_of_day;
            }

            if (sched.has_end && sched.end_hour == timeinfo.tm_hour && sched.end_minute == timeinfo.tm_min &&
                last_end_fired[i] != current_minute_of_day) {
                s_effects->stop();
                s_led->off();
                ESP_LOGI(TAG, "Horario #%d terminado -> %02d:%02d (apagado)", (int)i, sched.end_hour, sched.end_minute);
                last_end_fired[i] = current_minute_of_day;
            }
        }
    }
}

void start(Ws2812Led &led, EffectsManager &effects) {
    s_led = &led;
    s_effects = &effects;
    xTaskCreate(checkTaskFn, "scheduler_task", 4096, nullptr, 4, nullptr);
}

}  // namespace scheduler
