#pragma once
#include "Ws2812Led.hpp"
#include "Effects.hpp"

// Sincroniza la hora por internet (SNTP) y corre una tarea que revisa cada
// minuto si hay algún horario programado (guardado en NVS) que aplicar.
namespace scheduler {

void init_time();               // llamar una vez, después de conectar WiFi
void start(Ws2812Led &led, EffectsManager &effects); // arranca la tarea de chequeo
bool is_time_synced();

}  // namespace scheduler
