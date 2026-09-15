#include "Storage.hpp"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>
#include <algorithm>

namespace storage {

static const char *TAG = "storage";
static const char *NS_WIFI = "wifi_cfg";
static const char *NS_STATE = "led_state";
static const char *NS_SCENES = "scenes";
static const char *NS_SCHED = "schedules";
static const char *NS_SECURITY = "security";

void init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

bool load_wifi_credentials(std::string &ssid, std::string &pass) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READONLY, &h) != ESP_OK) return false;

    char ssid_buf[33] = {0};
    char pass_buf[65] = {0};
    size_t ssid_len = sizeof(ssid_buf);
    size_t pass_len = sizeof(pass_buf);

    esp_err_t r1 = nvs_get_str(h, "ssid", ssid_buf, &ssid_len);
    esp_err_t r2 = nvs_get_str(h, "pass", pass_buf, &pass_len);
    nvs_close(h);

    if (r1 != ESP_OK || r2 != ESP_OK || ssid_buf[0] == '\0') return false;

    ssid = ssid_buf;
    pass = pass_buf;
    return true;
}

void save_wifi_credentials(const std::string &ssid, const std::string &pass) {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo abrir NVS para guardar WiFi");
        return;
    }
    nvs_set_str(h, "ssid", ssid.c_str());
    nvs_set_str(h, "pass", pass.c_str());
    nvs_commit(h);
    nvs_close(h);
}

void clear_wifi_credentials() {
    nvs_handle_t h;
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

void save_last_color(uint8_t r, uint8_t g, uint8_t b) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t packed = (r << 16) | (g << 8) | b;
    nvs_set_u32(h, "color", packed);
    nvs_commit(h);
    nvs_close(h);
}

void load_last_color(uint8_t &r, uint8_t &g, uint8_t &b) {
    r = g = b = 0;
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t packed = 0;
    if (nvs_get_u32(h, "color", &packed) == ESP_OK) {
        r = (packed >> 16) & 0xFF;
        g = (packed >> 8) & 0xFF;
        b = packed & 0xFF;
    }
    nvs_close(h);
}

void save_last_effect(uint8_t effect_id) {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "effect", effect_id);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t load_last_effect() {
    nvs_handle_t h;
    if (nvs_open(NS_STATE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t effect_id = 0;
    nvs_get_u8(h, "effect", &effect_id);
    nvs_close(h);
    return effect_id;
}

// --- Escenas ---
std::vector<Scene> load_scenes() {
    std::vector<Scene> out;
    nvs_handle_t h;
    if (nvs_open(NS_SCENES, NVS_READONLY, &h) != ESP_OK) return out;

    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK) { nvs_close(h); return out; }
    if (count > kMaxScenes) count = kMaxScenes;

    Scene buf[kMaxScenes];
    size_t size = sizeof(buf);
    if (nvs_get_blob(h, "list", buf, &size) == ESP_OK) {
        for (uint8_t i = 0; i < count; i++) out.push_back(buf[i]);
    }
    nvs_close(h);
    return out;
}

static void write_scenes(const std::vector<Scene> &scenes) {
    nvs_handle_t h;
    if (nvs_open(NS_SCENES, NVS_READWRITE, &h) != ESP_OK) return;

    Scene buf[kMaxScenes] = {};
    uint8_t count = scenes.size() > kMaxScenes ? kMaxScenes : scenes.size();
    for (uint8_t i = 0; i < count; i++) buf[i] = scenes[i];

    nvs_set_u8(h, "count", count);
    nvs_set_blob(h, "list", buf, sizeof(buf));
    nvs_commit(h);
    nvs_close(h);
}

bool save_scene(const Scene &scene) {
    auto scenes = load_scenes();
    for (auto &s : scenes) {
        if (strncmp(s.name, scene.name, sizeof(s.name)) == 0) {
            s = scene; // actualiza si ya existe una con ese nombre
            write_scenes(scenes);
            return true;
        }
    }
    if (scenes.size() >= kMaxScenes) return false; // sin espacio
    scenes.push_back(scene);
    write_scenes(scenes);
    return true;
}

void delete_scene(const std::string &name) {
    auto scenes = load_scenes();
    scenes.erase(std::remove_if(scenes.begin(), scenes.end(), [&](const Scene &s) {
        return name == std::string(s.name);
    }), scenes.end());
    write_scenes(scenes);
}

// --- Horarios ---
std::vector<Schedule> load_schedules() {
    std::vector<Schedule> out;
    nvs_handle_t h;
    if (nvs_open(NS_SCHED, NVS_READONLY, &h) != ESP_OK) return out;

    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK) { nvs_close(h); return out; }
    if (count > kMaxSchedules) count = kMaxSchedules;

    Schedule buf[kMaxSchedules];
    size_t size = sizeof(buf);
    if (nvs_get_blob(h, "list", buf, &size) == ESP_OK) {
        for (uint8_t i = 0; i < count; i++) out.push_back(buf[i]);
    }
    nvs_close(h);
    return out;
}

void save_schedules(const std::vector<Schedule> &schedules) {
    nvs_handle_t h;
    if (nvs_open(NS_SCHED, NVS_READWRITE, &h) != ESP_OK) return;

    Schedule buf[kMaxSchedules] = {};
    uint8_t count = schedules.size() > kMaxSchedules ? kMaxSchedules : schedules.size();
    for (uint8_t i = 0; i < count; i++) buf[i] = schedules[i];

    nvs_set_u8(h, "count", count);
    nvs_set_blob(h, "list", buf, sizeof(buf));
    nvs_commit(h);
    nvs_close(h);
}

// --- Usuario y PIN de acceso ---
std::string load_username() {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READONLY, &h) != ESP_OK) return "admin";
    char buf[24] = {0};
    size_t len = sizeof(buf);
    esp_err_t r = nvs_get_str(h, "username", buf, &len);
    nvs_close(h);
    if (r != ESP_OK || buf[0] == '\0') return "admin";
    return buf;
}

void save_username(const std::string &username) {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "username", username.c_str());
    nvs_commit(h);
    nvs_close(h);
}

std::string load_pin() {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READONLY, &h) != ESP_OK) return "0000";
    char buf[16] = {0};
    size_t len = sizeof(buf);
    esp_err_t r = nvs_get_str(h, "pin", buf, &len);
    nvs_close(h);
    if (r != ESP_OK || buf[0] == '\0') return "0000";
    return buf;
}

void save_pin(const std::string &pin) {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "pin", pin.c_str());
    nvs_commit(h);
    nvs_close(h);
}

std::string load_ap_password() {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READONLY, &h) != ESP_OK) return "cambia-esta-clave";
    char buf[64] = {0};
    size_t len = sizeof(buf);
    esp_err_t r = nvs_get_str(h, "ap_pass", buf, &len);
    nvs_close(h);
    if (r != ESP_OK || buf[0] == '\0') return "cambia-esta-clave";
    return buf;
}

void save_ap_password(const std::string &password) {
    nvs_handle_t h;
    if (nvs_open(NS_SECURITY, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ap_pass", password.c_str());
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace storage
