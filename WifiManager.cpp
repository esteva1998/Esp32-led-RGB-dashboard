#include "WifiManager.hpp"
#include "Storage.hpp"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <cstring>

namespace {
constexpr int kConnectedBit = BIT0;
constexpr int kFailBit = BIT1;
const char *TAG = "wifi_mgr";
}

void WifiManager::eventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    auto *self = static_cast<WifiManager *>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(self->event_group_, kFailBit);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&event->ip_info.ip));
        self->ip_str_ = buf;
        ESP_LOGI(TAG, "IP obtenida: %s", buf);
        xEventGroupSetBits(self->event_group_, kConnectedBit);
    }
}

bool WifiManager::tryConnectSta(const std::string &ssid, const std::string &pass, uint32_t timeout_ms) {
    event_group_ = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t wifi_handler, ip_handler;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::eventHandler, this, &wifi_handler);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::eventHandler, this, &ip_handler);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass.c_str(), sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(event_group_, kConnectedBit | kFailBit,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler);

    return (bits & kConnectedBit) != 0;
}

void WifiManager::enablePermanentAP() {
    // Deja una red directa siempre disponible además de la conexión STA normal.
    esp_netif_create_default_wifi_ap();

    std::string ap_pass = storage::load_ap_password();

    wifi_config_t ap_config = {};
    strcpy((char *)ap_config.ap.ssid, "ESP32-LED-Direct");
    ap_config.ap.ssid_len = strlen("ESP32-LED-Direct");
    strncpy((char *)ap_config.ap.password, ap_pass.c_str(), sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    ESP_LOGI(TAG, "Red directa activa -> 'ESP32-LED-Direct', http://192.168.4.1 "
                  "(clave configurable desde el dashboard, no queda en el codigo fuente)");
}

namespace {

const char *PORTAL_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Configurar WiFi - ESP32</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2rem}
input{width:90%;max-width:280px;padding:.6rem;margin:.5rem;border-radius:6px;border:none}
button{padding:.7rem 2rem;background:#2980b9;color:#fff;border:none;border-radius:8px;font-weight:bold}
</style></head><body>
<h2>Configura el WiFi del ESP32</h2>
<form action="/wifi/save" method="POST">
<input name="ssid" placeholder="Nombre de red (SSID)" required><br>
<input name="pass" placeholder="Contraseña" type="password"><br>
<button type="submit">Guardar y conectar</button>
</form>
</body></html>
)HTML";

const char *CONNECTING_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Conectando...</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2rem}
a{color:#2980b9;font-size:1.2rem}
#spinner{font-size:2rem;margin:1rem}
</style></head><body>
<h2 id="msg">Conectando a tu red WiFi...</h2>
<div id="spinner">⏳</div>
<div id="result"></div>
<script>
async function poll() {
  try {
    const res = await fetch('/wifi/status');
    const data = await res.json();
    if (!data.done) { setTimeout(poll, 1200); return; }
    document.getElementById('spinner').textContent = '';
    if (data.ok) {
      document.getElementById('msg').textContent = '¡Conectado!';
      document.getElementById('result').innerHTML =
        `<p>IP nueva: <b>${data.ip}</b></p>` +
        `<p><a href="http://${data.ip}">Abrir dashboard: http://${data.ip}</a></p>` +
        `<p style="font-size:.8rem;color:#999">Tu celular se va a desconectar de esta red en unos segundos ` +
        `(la placa deja de usar "ESP32-LED-Setup"). Conéctate a tu red normal, o a "ESP32-LED-Direct", ` +
        `para usar el enlace de arriba.</p>`;
    } else {
      document.getElementById('msg').textContent = 'No se pudo conectar';
      document.getElementById('result').innerHTML =
        '<p>Revisa el nombre de red y la contraseña. La placa va a reiniciar el portal de configuración.</p>';
    }
  } catch (e) { setTimeout(poll, 1500); }
}
poll();
</script>
</body></html>
)HTML";

httpd_handle_t g_portal_server = nullptr;

volatile bool g_switch_in_progress = false;
volatile bool g_switch_done = false;
volatile bool g_switch_ok = false;
char g_switch_ip[16] = {0};
EventGroupHandle_t g_switch_events = nullptr;
constexpr int kSwitchConnectedBit = BIT0;
constexpr int kSwitchFailBit = BIT1;

esp_err_t portal_root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

// Decodifica application/x-www-form-urlencoded de forma mínima (solo %XX y '+')
std::string urlDecode(const std::string &in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            int val = strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            out += (char)val;
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

void switchEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(g_switch_events, kSwitchFailBit);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        snprintf(g_switch_ip, sizeof(g_switch_ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_switch_events, kSwitchConnectedBit);
    }
}

struct SwitchCreds { std::string ssid, pass; };

void switchTaskFn(void *arg) {
    // Deja que la respuesta HTTP de /wifi/save termine de salir antes de
    // tocar el driver WiFi (cambiar de modo lo corta brevemente y puede
    // resetear la conexión del navegador si se hace demasiado pronto).
    vTaskDelay(pdMS_TO_TICKS(800));

    auto *creds = static_cast<SwitchCreds *>(arg);
    std::string ssid = creds->ssid, pass = creds->pass;
    delete creds;

    g_switch_events = xEventGroupCreate();
    esp_event_handler_instance_t wifi_h, ip_h;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, switchEventHandler, nullptr, &wifi_h);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, switchEventHandler, nullptr, &ip_h);

    esp_netif_create_default_wifi_sta(); // agrega la interfaz STA (la AP del portal ya existe)

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass.c_str(), sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_APSTA); // conserva el AP del portal mientras intenta STA
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    EventBits_t bits = xEventGroupWaitBits(g_switch_events, kSwitchConnectedBit | kSwitchFailBit,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_h);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_h);

    if (bits & kSwitchConnectedBit) {
        storage::save_wifi_credentials(ssid, pass);
        g_switch_ok = true;
    } else {
        g_switch_ok = false;
    }
    g_switch_done = true;
    g_switch_in_progress = false;
    vTaskDelete(nullptr);
}

esp_err_t portal_save_handler(httpd_req_t *req) {
    if (g_switch_in_progress) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, CONNECTING_HTML, HTTPD_RESP_USE_STRLEN);
    }

    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char ssid_raw[64] = {0}, pass_raw[128] = {0};
    httpd_query_key_value(buf, "ssid", ssid_raw, sizeof(ssid_raw));
    httpd_query_key_value(buf, "pass", pass_raw, sizeof(pass_raw));

    auto *creds = new SwitchCreds{ urlDecode(ssid_raw), urlDecode(pass_raw) };

    g_switch_done = false;
    g_switch_ok = false;
    g_switch_ip[0] = '\0';
    g_switch_in_progress = true;
    xTaskCreate(switchTaskFn, "wifi_switch", 4096, creds, 5, nullptr);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, CONNECTING_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t portal_status_handler(httpd_req_t *req) {
    char json[96];
    snprintf(json, sizeof(json), "{\"done\":%s,\"ok\":%s,\"ip\":\"%s\"}",
             g_switch_done ? "true" : "false",
             g_switch_ok ? "true" : "false",
             g_switch_ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void WifiManager::startConfigPortal() {
    ESP_LOGW(TAG, "Sin credenciales validas -> iniciando portal de configuracion");

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {};
    strcpy((char *)ap_config.ap.ssid, "ESP32-LED-Setup");
    ap_config.ap.ssid_len = strlen("ESP32-LED-Setup");
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGW(TAG, "Conectate a la red 'ESP32-LED-Setup' (sin clave) y abre http://192.168.4.1");

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&g_portal_server, &http_cfg);
    httpd_uri_t root_uri   = { "/", HTTP_GET, portal_root_handler, nullptr };
    httpd_uri_t save_uri   = { "/wifi/save", HTTP_POST, portal_save_handler, nullptr };
    httpd_uri_t status_uri = { "/wifi/status", HTTP_GET, portal_status_handler, nullptr };
    httpd_register_uri_handler(g_portal_server, &root_uri);
    httpd_register_uri_handler(g_portal_server, &save_uri);
    httpd_register_uri_handler(g_portal_server, &status_uri);

    // Espera bloqueante (sin trabar la CPU) hasta que el intento de conexión termine.
    while (!g_switch_done) {
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (g_switch_ok) {
        ip_str_ = g_switch_ip;
        ESP_LOGI(TAG, "Conectado en vivo, IP: %s. Esperando a que el navegador confirme...", ip_str_.c_str());
        // Da tiempo a que el navegador (todavía conectado a 'ESP32-LED-Setup')
        // reciba la confirmación final antes de que esa red desaparezca.
        vTaskDelay(pdMS_TO_TICKS(6000));
        httpd_stop(g_portal_server);
        enablePermanentAP();
    } else {
        ESP_LOGW(TAG, "Fallo la conexion con las credenciales ingresadas, reiniciando portal...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart(); // reinicia limpio y vuelve a mostrar el portal
    }
}

void WifiManager::begin() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    std::string ssid, pass;
    bool has_creds = storage::load_wifi_credentials(ssid, pass);

    if (has_creds) {
        ESP_LOGI(TAG, "Probando credenciales guardadas para '%s'...", ssid.c_str());
        if (tryConnectSta(ssid, pass, 30000)) {
            enablePermanentAP();
            return;
        }
        ESP_LOGW(TAG, "No se pudo conectar con las credenciales guardadas.");
    }

    startConfigPortal(); // ahora SÍ retorna si logra conectar en vivo (ya no reinicia en el caso exitoso)
}
