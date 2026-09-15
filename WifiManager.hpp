#pragma once
#include <string>
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Intenta conectar con credenciales guardadas en NVS. Si no hay credenciales
// o falla la conexión, levanta un Access Point ("ESP32-LED-Setup") con un
// formulario web simple para que el usuario ingrese su WiFi. Al guardar,
// la placa reinicia y se conecta con las nuevas credenciales.
class WifiManager {
public:
    void begin(); // bloquea hasta que hay conexión STA (puede reiniciar antes)
    std::string ip() const { return ip_str_; }

private:
    bool tryConnectSta(const std::string &ssid, const std::string &pass, uint32_t timeout_ms);
    void startConfigPortal();
    void enablePermanentAP();

    static void eventHandler(void *arg, esp_event_base_t base, int32_t id, void *data);

    EventGroupHandle_t event_group_ = nullptr;
    std::string ip_str_;
};
