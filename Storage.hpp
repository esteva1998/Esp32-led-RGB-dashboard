#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Guarda/lee en NVS (flash no volátil): credenciales WiFi, último color/efecto,
// escenas guardadas, horarios programados y el PIN de acceso.
namespace storage {

void init();

// --- WiFi ---
bool load_wifi_credentials(std::string &ssid, std::string &pass);
void save_wifi_credentials(const std::string &ssid, const std::string &pass);
void clear_wifi_credentials();

// --- Último estado ---
void save_last_color(uint8_t r, uint8_t g, uint8_t b);
void load_last_color(uint8_t &r, uint8_t &g, uint8_t &b);
void save_last_effect(uint8_t effect_id);
uint8_t load_last_effect();

// --- Escenas guardadas ---
struct Scene {
    char name[16];
    uint8_t step_count;
    uint8_t colors[6][3]; // hasta 6 colores en la secuencia
};
constexpr size_t kMaxSceneSteps = 6;
constexpr size_t kMaxScenes = 8;

std::vector<Scene> load_scenes();
bool save_scene(const Scene &scene);          // agrega o actualiza (por nombre)
void delete_scene(const std::string &name);

// --- Horarios programados ---
struct Schedule {
    uint8_t start_hour, start_minute;
    uint8_t end_hour, end_minute; // solo se usa si has_end == true
    bool has_end;                 // si tiene hora de fin, se apaga sola al llegar
    uint8_t action;   // 0 = color fijo, 1 = efecto, 2 = escena
    uint8_t r, g, b;
    uint8_t effect;
    char scene_name[16];
    bool enabled;
};
constexpr size_t kMaxSchedules = 6;

std::vector<Schedule> load_schedules();
void save_schedules(const std::vector<Schedule> &schedules); // reemplaza la lista completa

// --- PIN de acceso ---
// --- Usuario y PIN de acceso ---
std::string load_username();     // "admin" por defecto si nunca se configuró
void save_username(const std::string &username);
std::string load_pin();          // "0000" por defecto si nunca se configuró
void save_pin(const std::string &pin);

// --- Contraseña de la red directa "ESP32-LED-Direct" ---
std::string load_ap_password();  // valor por defecto si nunca se configuró (cámbialo)
void save_ap_password(const std::string &password);

}  // namespace storage
