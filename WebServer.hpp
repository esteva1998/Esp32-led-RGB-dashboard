#pragma once
#include "Ws2812Led.hpp"
#include "Effects.hpp"
#include <string>

// Registra y arranca el servidor HTTP principal (post-conexión WiFi):
// "/"        -> dashboard
// "/led"     -> set color manual (también cancela el efecto activo)
// "/effect"  -> activa/desactiva un efecto automático
// "/status"  -> JSON con uptime, heap libre, RSSI, IP, SSID, color y efecto actual
// "/history" -> JSON con el registro de eventos recientes
// "/ota"     -> sube un nuevo firmware .bin (POST, cuerpo binario crudo)
void start_main_webserver(Ws2812Led &led, EffectsManager &effects, const std::string &ip);

// Aplica (reproduce) una escena guardada por nombre. La usan tanto el dashboard
// como el Scheduler (para horarios que disparan una escena).
void apply_scene_by_name(const std::string &name);
