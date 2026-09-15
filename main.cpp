// main.cpp — Dashboard web para controlar el LED RGB (WS2812) del ESP32-S3 N16R8
// Framework: ESP-IDF puro (sin Arduino), compilado vía PlatformIO.
//
// Módulos:
//   Ws2812Led    - driver RMT del LED (RAII, sin componentes externos)
//   Effects      - efectos automáticos (arcoíris, respiración, estroboscopio)
//   WifiManager  - conexión WiFi con portal de configuración de respaldo
//   Storage      - NVS: credenciales WiFi, último color/efecto
//   WebServer    - dashboard, /led, /effect, /status, /ota

#include "Ws2812Led.hpp"
#include "Effects.hpp"
#include "WifiManager.hpp"
#include "Storage.hpp"
#include "WebServer.hpp"
#include "Scheduler.hpp"

#include "esp_log.h"
#include "mdns.h"

static const char *TAG = "main";
static const char *MDNS_HOSTNAME = "led-dashboard"; // -> http://led-dashboard.local

static void start_mdns() {
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Dashboard LED RGB ESP32-S3"));
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(TAG, "mDNS activo -> http://%s.local", MDNS_HOSTNAME);
}

extern "C" void app_main(void) {
    storage::init();

    static Ws2812Led led(GPIO_NUM_48);
    led.off();

    // Restaura el último color guardado (si había alguno)
    uint8_t r, g, b;
    storage::load_last_color(r, g, b);
    if (r || g || b) led.setColor(r, g, b);

    static EffectsManager effects(led);
    uint8_t saved_effect = storage::load_last_effect();
    if (saved_effect != 0) effects.start(static_cast<EffectMode>(saved_effect));

    static WifiManager wifi;
    wifi.begin(); // bloquea hasta conectar STA (o reinicia si se configura por el portal)

    scheduler::init_time();
    scheduler::start(led, effects);

    start_mdns();
    start_main_webserver(led, effects, wifi.ip());

    ESP_LOGI(TAG, "Listo -> http://%s / http://%s.local", wifi.ip().c_str(), MDNS_HOSTNAME);
}
