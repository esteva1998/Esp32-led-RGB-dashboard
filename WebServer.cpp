#include "WebServer.hpp"
#include "Storage.hpp"
#include "Scheduler.hpp"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <ctime>
#include <algorithm>
#include <cctype>

namespace {

const char *TAG = "webserver";

Ws2812Led *g_led = nullptr;
EffectsManager *g_effects = nullptr;
bool g_power_save = false;
std::string g_ip_str;

// Reproducción de escenas (secuencias de colores)
std::vector<std::array<uint8_t, 3>> g_scene_colors;
volatile bool g_scene_playing = false;
std::string g_active_scene_name;

// Historial de eventos (se borra al entrar en modo ahorro)
constexpr size_t kMaxHistory = 20;
std::deque<std::string> g_history;

void addHistory(const std::string &msg) {
    char line[96];
    snprintf(line, sizeof(line), "[%llds] %s", (long long)(esp_timer_get_time() / 1000000), msg.c_str());
    g_history.push_back(line);
    if (g_history.size() > kMaxHistory) g_history.pop_front();
}

// --- Sesiones con cookie (reemplaza HTTP Basic Auth) ---
// Con bloqueo temporal tras intentos fallidos repetidos en /login (defensa contra fuerza bruta).
int g_failed_auth_attempts = 0;
int64_t g_auth_lockout_until_us = 0;
constexpr int kMaxFailedAttempts = 5;
constexpr int64_t kLockoutDurationUs = 60LL * 1000000; // 60 segundos

struct Session {
    char token[33] = {0};
    int64_t expires_at_us = 0;
    bool active = false;
};
constexpr int kMaxSessions = 4; // hasta 4 dispositivos/pestañas con sesión activa a la vez
constexpr int64_t kSessionDurationUs = 24LL * 3600 * 1000000; // 24 horas
Session g_sessions[kMaxSessions];

std::string generateSessionToken() {
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw)); // generador aleatorio por hardware del ESP32
    static const char hexch[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint8_t b : raw) {
        out += hexch[b >> 4];
        out += hexch[b & 0xF];
    }
    return out;
}

void createSession(const std::string &token) {
    int64_t now = esp_timer_get_time();
    int slot = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < kMaxSessions; i++) {
        if (!g_sessions[i].active) { slot = i; break; }
        if (g_sessions[i].expires_at_us < oldest) { oldest = g_sessions[i].expires_at_us; slot = i; }
    }
    strncpy(g_sessions[slot].token, token.c_str(), sizeof(g_sessions[slot].token) - 1);
    g_sessions[slot].expires_at_us = now + kSessionDurationUs;
    g_sessions[slot].active = true;
}

bool validateSessionToken(const std::string &token) {
    if (token.empty()) return false;
    int64_t now = esp_timer_get_time();
    for (auto &s : g_sessions) {
        if (s.active && token == s.token) {
            if (now > s.expires_at_us) { s.active = false; return false; }
            return true;
        }
    }
    return false;
}

void destroySessionToken(const std::string &token) {
    for (auto &s : g_sessions) {
        if (s.active && token == s.token) s.active = false;
    }
}

std::string getCookieValue(httpd_req_t *req, const std::string &name) {
    char cookie_hdr[256] = {0};
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) return "";
    std::string cookies(cookie_hdr);
    std::string key = name + "=";
    size_t pos = cookies.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = cookies.find(';', pos);
    return cookies.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

bool checkSession(httpd_req_t *req) {
    return validateSessionToken(getCookieValue(req, "session"));
}

// Valida usuario+PIN contra lo guardado (usado solo en /login). Aplica el
// bloqueo temporal por fuerza bruta.
bool checkCredentials(const std::string &user, const std::string &pin) {
    int64_t now = esp_timer_get_time();
    if (now < g_auth_lockout_until_us) return false;

    bool ok = (user == storage::load_username() && pin == storage::load_pin());
    if (ok) {
        g_failed_auth_attempts = 0;
    } else {
        g_failed_auth_attempts++;
        if (g_failed_auth_attempts >= kMaxFailedAttempts) {
            g_auth_lockout_until_us = now + kLockoutDurationUs;
            g_failed_auth_attempts = 0;
            ESP_LOGW(TAG, "Bloqueo temporal de acceso por intentos fallidos repetidos");
            addHistory("Bloqueo temporal: demasiados intentos de acceso fallidos");
        }
    }
    return ok;
}

// --- Protección CSRF básica: rechaza peticiones que el navegador marca como
// "cross-site" (venidas de otra página web, no del propio dashboard). Es una
// capa extra: la cookie de sesión ya usa SameSite=Strict, que por sí solo
// evita que se envíe en peticiones de otro sitio en navegadores modernos. ---
bool isSameSiteRequest(httpd_req_t *req) {
    char site[16] = {0};
    if (httpd_req_get_hdr_value_str(req, "Sec-Fetch-Site", site, sizeof(site)) == ESP_OK) {
        return std::string(site) != "cross-site";
    }
    return true; // navegadores viejos sin este header: se deja pasar (no se puede verificar)
}

esp_err_t sendAuthChallenge(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Sesion invalida o expirada", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t sendCsrfBlocked(httpd_req_t *req) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_send(req, "Peticion bloqueada (origen no confiable)", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#define REQUIRE_AUTH(req) \
    if (!checkSession(req)) return sendAuthChallenge(req); \
    if (!isSameSiteRequest(req)) return sendCsrfBlocked(req);

const char *DASHBOARD_HTML = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>Control LED RGB - ESP32-S3</title>
<style>
  * { box-sizing: border-box; }
  html, body {
    height: 100%; margin: 0; font-family: sans-serif; background:#111; color:#eee;
    overflow: hidden;
  }
  body {
    display: flex; flex-direction: column; padding: .6rem; gap: .5rem;
    font-size: 13px;
  }
  h1 { font-size: 1rem; margin: 0; text-align:center; }
  h3 { font-size: .7rem; color:#888; margin: 0 0 .3rem; text-transform: uppercase; letter-spacing:.05em; }

  #default-creds-warning {
    display:none; background:#4a2e0e; color:#ffcf7a; border:1px solid #e08a1e; border-radius:6px;
    padding:.4rem .6rem; font-size:.7rem; text-align:center;
  }
  #default-creds-warning.show { display:block; }
  #default-creds-warning a { color:#fff; font-weight:bold; }

  #info-bar {
    display:flex; justify-content:space-between; gap:.4rem; background:#1a1a1a;
    border-radius:8px; padding:.4rem .6rem; font-size:.7rem; color:#aaa; flex-wrap:wrap;
  }
  #info-bar span { white-space: nowrap; }

  #main-row { display:flex; gap:.8rem; align-items:stretch; flex: 1; min-height:0; }
  #left-col { flex: 1; min-width:0; display:flex; flex-direction:column; gap:.5rem; }
  #right-col { width: 130px; flex-shrink:0; display:flex; flex-direction:column; gap:.4rem; }

  #color-row { display:flex; align-items:center; gap:.8rem; }
  #preview { width:64px; height:64px; border-radius:50%; border:3px solid #444; flex-shrink:0; }
  input[type=color] { width:50px; height:50px; border:none; border-radius:10px; background:none; flex-shrink:0; }
  #sliders { flex:1; }
  .slider-row { margin-bottom:.3rem; }
  .slider-row label { display:flex; justify-content:space-between; font-size:.7rem; font-weight:bold; margin-bottom:.1rem; }
  input[type=range] { width:100%; height:1.1rem; }
  #r-label { color:#ff6b6b; } #g-label { color:#6bff8f; } #b-label { color:#6b9bff; }

  #off-btn { padding:.4rem; font-size:.75rem; font-weight:bold; background:#c0392b; color:#fff; border:none; border-radius:6px; }

  #palette { display:flex; gap:.3rem; overflow-x:auto; padding-bottom:.2rem; }
  .swatch { width:38px; height:38px; flex-shrink:0; border-radius:6px; border:2px solid #333; padding:0; }

  #effects, #config { display:flex; flex-direction:column; gap:.3rem; }
  .effect-btn, .config-btn {
    padding:.35rem; border-radius:6px; border:2px solid #333; background:#222; color:#eee;
    font-size:.7rem; cursor:pointer;
  }
  .effect-btn.active { border-color:#2980b9; background:#1c3d52; }
  #wifi-change-btn { background:#2980b9; border-color:#2980b9; color:#fff; }
  #wifi-forget-btn { background:#7f8c8d; border-color:#7f8c8d; color:#fff; }
  #powersave-btn { background:#4a0e0e; border-color:#4a0e0e; color:#fff; }
  #powersave-btn.active { background:#27ae60; border-color:#27ae60; }

  #history-box {
    flex: 3; min-height: 0; background:#1a1a1a; border-radius:8px; padding:.4rem .6rem;
    overflow-y: auto; font-size:.7rem; color:#aaa; font-family: monospace;
  }
  #history-box div { padding: .15rem 0; border-bottom: 1px solid #262626; }

  #scenes-box { display:flex; gap:.3rem; flex-wrap:nowrap; overflow-x:auto; padding-bottom:.2rem; }
  .scene-chip {
    display:flex; align-items:center; gap:.3rem; background:#222; border:1px solid #333;
    border-radius:14px; padding:.2rem .5rem; font-size:.65rem; cursor:pointer; flex-shrink:0;
  }
  .scene-chip .del { color:#c0392b; font-weight:bold; padding-left:.2rem; }

  #locked-overlay {
    display:none; position:fixed; inset:0; background:rgba(0,0,0,.75); z-index:10;
    flex-direction:column; align-items:center; justify-content:center; text-align:center; padding:2rem;
  }
  #locked-overlay.show { display:flex; }
  #locked-overlay button {
    margin-top:1.2rem; padding:.7rem 1.6rem; background:#27ae60; color:#fff; border:none;
    border-radius:8px; font-weight:bold; font-size:.9rem;
  }

  #builder-overlay {
    display:none; position:fixed; inset:0; background:rgba(0,0,0,.85); z-index:11;
    flex-direction:column; align-items:center; justify-content:center; text-align:center; padding:1.5rem;
  }
  #builder-overlay.show { display:flex; }
  #builder-overlay h3 { color:#eee; font-size:1rem; margin-bottom:.8rem; text-transform:none; letter-spacing:0; }
  #builder-overlay input[type=color] {
    width:80px; height:60px; border:none; border-radius:10px; background:none; margin-bottom:1rem; cursor:pointer;
  }
  #builder-steps { display:flex; gap:.3rem; flex-wrap:wrap; justify-content:center; max-width:320px; margin-bottom:1rem; min-height:36px; }
  .builder-step { width:32px; height:32px; border-radius:6px; border:2px solid #444; position:relative; }
  .builder-step .rm {
    position:absolute; top:-8px; right:-8px; background:#c0392b; color:#fff; border-radius:50%;
    width:16px; height:16px; font-size:.6rem; line-height:16px; cursor:pointer;
  }
  #builder-overlay input[type=text] {
    padding:.6rem; border-radius:6px; border:none; width:220px; text-align:center; margin-bottom:.8rem;
  }
  #builder-overlay button.action {
    padding:.6rem 1.2rem; border-radius:8px; border:none; font-weight:bold; margin:.2rem; font-size:.85rem;
  }
  #add-step-btn { background:#2980b9; color:#fff; }
  #save-scene-btn { background:#27ae60; color:#fff; }
  #cancel-scene-btn { background:#7f8c8d; color:#fff; }
</style>
</head>
<body>
  <h1>LED RGB - ESP32-S3</h1>

  <div id="default-creds-warning">⚠ Sigues usando el usuario/PIN por defecto (admin/0000). <a href="/pin">Cámbialo ahora</a>.</div>

  <div id="info-bar">
    <span id="info-ip">IP: -</span>
    <span id="info-ssid">Red: -</span>
    <span id="info-uptime">Uptime: -</span>
    <span id="info-heap">RAM: -</span>
    <span id="info-rssi">Señal: -</span>
    <span id="info-scene"></span>
  </div>

  <div id="main-row">
    <div id="left-col">
      <div id="color-row">
        <div id="preview"></div>
        <input type="color" id="picker" value="#000000">
        <div id="sliders">
          <div class="slider-row">
            <label id="r-label"><span>Rojo</span><span id="r-val">0</span></label>
            <input type="range" id="r" min="0" max="255" value="0">
          </div>
          <div class="slider-row">
            <label id="g-label"><span>Verde</span><span id="g-val">0</span></label>
            <input type="range" id="g" min="0" max="255" value="0">
          </div>
          <div class="slider-row">
            <label id="b-label"><span>Azul</span><span id="b-val">0</span></label>
            <input type="range" id="b" min="0" max="255" value="0">
          </div>
        </div>
      </div>
      <button id="off-btn">Apagar LED</button>

      <h3>Colores rápidos</h3>
      <div id="palette"></div>

      <h3>Escenas <span id="scene-add-btn" style="cursor:pointer;color:#2980b9;">＋ nueva</span></h3>
      <div id="scenes-box"></div>

      <h3>Historial</h3>
      <div id="history-box"></div>
    </div>

    <div id="right-col">
      <h3>Efectos</h3>
      <div id="effects">
        <button class="effect-btn" data-effect="1">Arcoíris</button>
        <button class="effect-btn" data-effect="2">Respiración</button>
        <button class="effect-btn" data-effect="3">Estroboscopio</button>
      </div>

      <h3>Config</h3>
      <div id="config">
        <button class="config-btn" id="wifi-change-btn">Cambiar WiFi</button>
        <button class="config-btn" id="wifi-forget-btn">Olvidar WiFi</button>
        <button class="config-btn" id="powersave-btn">Ahorro energía</button>
        <button class="config-btn" onclick="window.location.href='/schedule'">Horario</button>
        <button class="config-btn" onclick="window.location.href='/pin'">Cambiar PIN</button>
        <button class="config-btn" onclick="window.location.href='/ap-password'">Clave red directa</button>
        <button class="config-btn" id="logout-btn">Cerrar sesión</button>
      </div>
    </div>
  </div>

  <div id="locked-overlay">
    <div>
      <p>El dashboard está bloqueado: el ESP32 está en modo de ahorro de energía.</p>
      <button id="unlock-btn">Reactivar</button>
    </div>
  </div>

  <div id="builder-overlay">
    <h3>Nueva escena (secuencia de colores)</h3>
    <p style="color:#999;font-size:.75rem;max-width:280px">
      Elige un color y toca "Agregar color" cada vez. Cuando termines la secuencia, ponle un nombre y guarda.
    </p>
    <input type="color" id="builder-color-picker" value="#ff0000">
    <div id="builder-steps"></div>
    <button class="action" id="add-step-btn">Agregar color</button><br>
    <input type="text" id="scene-name-input" placeholder="Nombre de la escena" maxlength="15">
    <div>
      <button class="action" id="save-scene-btn">Guardar secuencia</button>
      <button class="action" id="cancel-scene-btn">Cancelar</button>
    </div>
  </div>

<script>
  // Si la sesión venció o es inválida, cualquier petición devuelve 401 ->
  // mandamos directo al login en vez de dejar la página rota en silencio.
  const _origFetch = window.fetch;
  window.fetch = function(...args) {
    return _origFetch(...args).then(res => {
      if (res.status === 401) window.location.href = '/login';
      return res;
    });
  };

  const r = document.getElementById('r');
  const g = document.getElementById('g');
  const b = document.getElementById('b');
  const preview = document.getElementById('preview');
  const offBtn = document.getElementById('off-btn');
  const palette = document.getElementById('palette');
  const picker = document.getElementById('picker');
  const psBtn = document.getElementById('powersave-btn');
  const overlay = document.getElementById('locked-overlay');
  const historyBox = document.getElementById('history-box');

  const PRESET_COLORS = [
    [255,0,0],[0,255,0],[0,0,255],[255,255,255],[255,255,0],[0,255,255],
    [255,0,255],[255,128,0],[128,0,255],[255,0,128],[0,128,255],[128,255,0],
  ];

  PRESET_COLORS.forEach(([pr, pg, pb]) => {
    const btn = document.createElement('button');
    btn.className = 'swatch';
    btn.style.background = `rgb(${pr}, ${pg}, ${pb})`;
    btn.addEventListener('click', () => { r.value=pr; g.value=pg; b.value=pb; sendColor(); });
    palette.appendChild(btn);
  });

  function toHex(v) { return parseInt(v).toString(16).padStart(2, '0'); }

  function updatePreview() {
    preview.style.background = `rgb(${r.value}, ${g.value}, ${b.value})`;
    document.getElementById('r-val').textContent = r.value;
    document.getElementById('g-val').textContent = g.value;
    document.getElementById('b-val').textContent = b.value;
    picker.value = `#${toHex(r.value)}${toHex(g.value)}${toHex(b.value)}`;
  }

  function clearActiveEffect() {
    document.querySelectorAll('.effect-btn').forEach(bt => bt.classList.remove('active'));
  }

  let pending = false;
  let queuedNext = false;
  function sendColor() {
    updatePreview();
    clearActiveEffect();
    if (pending) { queuedNext = true; return; }
    pending = true;
    fetch(`/led?r=${r.value}&g=${g.value}&b=${b.value}`).finally(() => {
      pending = false;
      if (queuedNext) { queuedNext = false; sendColor(); } // manda la última petición que quedó pendiente
    });
  }

  [r, g, b].forEach(el => el.addEventListener('input', sendColor));

  picker.addEventListener('input', () => {
    const hex = picker.value;
    r.value = parseInt(hex.substr(1,2), 16);
    g.value = parseInt(hex.substr(3,2), 16);
    b.value = parseInt(hex.substr(5,2), 16);
    sendColor();
  });

  offBtn.addEventListener('click', () => { r.value=0; g.value=0; b.value=0; sendColor(); });

  document.querySelectorAll('.effect-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const isActive = btn.classList.contains('active');
      clearActiveEffect();
      const effectId = isActive ? 0 : btn.dataset.effect;
      if (!isActive) btn.classList.add('active');
      fetch(`/effect?id=${effectId}`);
    });
  });

  document.getElementById('wifi-change-btn').addEventListener('click', () => { window.location.href = '/wifi'; });

  document.getElementById('wifi-forget-btn').addEventListener('click', () => {
    if (!confirm('¿Olvidar la red WiFi guardada? La placa va a reiniciar y crear "ESP32-LED-Setup" para reconfigurarla.')) return;
    fetch('/wifi/forget', { method: 'POST' });
  });

  async function togglePowerSave(turnOn) {
    await fetch(turnOn ? '/powersave/on' : '/powersave/off', { method: 'POST' });
    refreshStatus();
  }
  psBtn.addEventListener('click', () => togglePowerSave(!psBtn.classList.contains('active')));
  document.getElementById('unlock-btn').addEventListener('click', () => togglePowerSave(false));

  async function refreshHistory() {
    try {
      const res = await fetch('/history');
      const data = await res.json();
      historyBox.innerHTML = data.length
        ? data.slice().reverse().map(l => `<div>${l}</div>`).join('')
        : '<div style="opacity:.5">Sin eventos aún</div>';
    } catch (e) { /* silencioso */ }
  }

  const scenesBox = document.getElementById('scenes-box');

  async function refreshScenes() {
    try {
      const res = await fetch('/scenes');
      const scenes = await res.json();
      scenesBox.innerHTML = '';
      scenes.forEach(sc => {
        const chip = document.createElement('div');
        chip.className = 'scene-chip';
        const first = sc.colors && sc.colors.length ? sc.colors[0] : [0,0,0];
        chip.style.borderLeft = `4px solid rgb(${first[0]},${first[1]},${first[2]})`;
        const stepCount = sc.colors ? sc.colors.length : 0;

        const nameSpan = document.createElement('span');
        nameSpan.textContent = `${sc.name} (${stepCount})`; // textContent: nunca interpreta HTML/JS
        const delSpan = document.createElement('span');
        delSpan.className = 'del';
        delSpan.textContent = '✕';
        chip.appendChild(nameSpan);
        chip.appendChild(delSpan);

        nameSpan.addEventListener('click', () => {
          clearActiveEffect();
          fetch(`/scenes/play?name=${encodeURIComponent(sc.name)}`, { method: 'POST' });
        });
        delSpan.addEventListener('click', (ev) => {
          ev.stopPropagation();
          if (!confirm(`¿Borrar la escena "${sc.name}"?`)) return;
          fetch(`/scenes/delete?name=${encodeURIComponent(sc.name)}`, { method: 'POST' }).then(refreshScenes);
        });
        scenesBox.appendChild(chip);
      });
    } catch (e) { /* silencioso */ }
  }

  // --- Constructor de escenas (secuencia de colores) ---
  const builderOverlay = document.getElementById('builder-overlay');
  const builderSteps = document.getElementById('builder-steps');
  const sceneNameInput = document.getElementById('scene-name-input');
  let tempSteps = [];

  function renderBuilderSteps() {
    builderSteps.innerHTML = '';
    tempSteps.forEach((step, i) => {
      const sw = document.createElement('div');
      sw.className = 'builder-step';
      sw.style.background = `rgb(${step[0]},${step[1]},${step[2]})`;
      sw.innerHTML = '<span class="rm">✕</span>';
      sw.querySelector('.rm').addEventListener('click', () => {
        tempSteps.splice(i, 1);
        renderBuilderSteps();
      });
      builderSteps.appendChild(sw);
    });
  }

  document.getElementById('scene-add-btn').addEventListener('click', () => {
    tempSteps = [];
    sceneNameInput.value = '';
    renderBuilderSteps();
    builderOverlay.classList.add('show');
  });

  document.getElementById('add-step-btn').addEventListener('click', () => {
    const hex = document.getElementById('builder-color-picker').value;
    const rr = parseInt(hex.substr(1,2), 16);
    const gg = parseInt(hex.substr(3,2), 16);
    const bb = parseInt(hex.substr(5,2), 16);
    tempSteps.push([rr, gg, bb]);
    renderBuilderSteps();
  });

  document.getElementById('cancel-scene-btn').addEventListener('click', () => {
    builderOverlay.classList.remove('show');
  });

  document.getElementById('save-scene-btn').addEventListener('click', async () => {
    const name = sceneNameInput.value.trim();
    if (!name) { alert('Ponle un nombre a la escena.'); return; }
    if (tempSteps.length === 0) { alert('Agrega al menos un color a la secuencia.'); return; }
    await fetch('/scenes/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, colors: tempSteps })
    });
    builderOverlay.classList.remove('show');
    refreshScenes();
  });

  async function refreshStatus() {
    try {
      const res = await fetch('/status');
      const data = await res.json();
      document.getElementById('info-ip').textContent = `IP: ${data.ip}`;
      document.getElementById('info-ssid').textContent = `Red: ${data.ssid}`;
      document.getElementById('info-uptime').textContent = `Uptime: ${data.uptime_s}s`;
      document.getElementById('info-heap').textContent = `RAM: ${Math.round(data.free_heap/1024)}KB`;
      document.getElementById('info-rssi').textContent = `Señal: ${data.rssi}dBm`;
      document.getElementById('info-scene').textContent = data.active_scene ? `Escena: ${data.active_scene}` : '';
      document.getElementById('default-creds-warning').classList.toggle('show', data.using_default_creds);

      psBtn.textContent = data.power_save ? 'Reactivar' : 'Ahorro energía';
      psBtn.classList.toggle('active', data.power_save);
      overlay.classList.toggle('show', data.power_save);
    } catch (e) { /* silencioso */ }
  }

  refreshStatus();
  refreshHistory();
  refreshScenes();
  setInterval(refreshStatus, 4000);
  setInterval(refreshHistory, 4000);

  document.getElementById('logout-btn').addEventListener('click', async () => {
    await fetch('/logout', { method: 'POST' });
    window.location.href = '/login';
  });

  updatePreview();
</script>
</body>
</html>
)HTML";

esp_err_t root_get_handler(httpd_req_t *req) {
    if (!checkSession(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        return httpd_resp_send(req, nullptr, 0);
    }
    if (!isSameSiteRequest(req)) return sendCsrfBlocked(req);
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY"); // evita que otra web te embeba el dashboard en un iframe
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

int get_query_param_int(const char *buf, const char *key, int fallback) {
    char value[8];
    if (httpd_query_key_value(buf, key, value, sizeof(value)) == ESP_OK) {
        return atoi(value);
    }
    return fallback;
}

esp_err_t led_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    if (g_power_save) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_send(req, "En modo ahorro de energia", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char buf[64];
    int r = 0, g = 0, b = 0;
    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len > 1 && qs_len <= sizeof(buf) && httpd_req_get_url_query_str(req, buf, qs_len) == ESP_OK) {
        r = get_query_param_int(buf, "r", 0);
        g = get_query_param_int(buf, "g", 0);
        b = get_query_param_int(buf, "b", 0);
    }
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);

    g_effects->stop(); // el control manual siempre cancela el efecto activo
    g_scene_playing = false; g_active_scene_name.clear();
    g_led->setColor((uint8_t)r, (uint8_t)g, (uint8_t)b);
    storage::save_last_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
    storage::save_last_effect(0);

    char msg[64];
    snprintf(msg, sizeof(msg), "Color -> R:%d G:%d B:%d", r, g, b);
    addHistory(msg);
    ESP_LOGI(TAG, "%s", msg);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

esp_err_t effect_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    if (g_power_save) {
        httpd_resp_set_status(req, "423 Locked");
        httpd_resp_send(req, "En modo ahorro de energia", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char buf[32];
    int id = 0;
    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len > 1 && qs_len <= sizeof(buf) && httpd_req_get_url_query_str(req, buf, qs_len) == ESP_OK) {
        id = get_query_param_int(buf, "id", 0);
    }
    g_effects->start(static_cast<EffectMode>(id));
    g_scene_playing = false; g_active_scene_name.clear();
    storage::save_last_effect((uint8_t)id);

    const char *names[] = {"Ninguno", "Arcoiris", "Respiracion", "Estroboscopio"};
    char msg[48];
    snprintf(msg, sizeof(msg), "Efecto -> %s", (id >= 0 && id <= 3) ? names[id] : "?");
    addHistory(msg);
    ESP_LOGI(TAG, "%s", msg);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

esp_err_t status_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    wifi_ap_record_t ap_info = {};
    int rssi = 0;
    std::string ssid = "-";
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
        ssid = std::string(reinterpret_cast<const char *>(ap_info.ssid));
    }

    auto color = g_led->currentColor();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "ip", g_ip_str.c_str());
    cJSON_AddStringToObject(root, "ssid", ssid.c_str());
    cJSON_AddNumberToObject(root, "effect", (int)g_effects->current());
    cJSON_AddBoolToObject(root, "power_save", g_power_save);
    bool using_defaults = (storage::load_username() == "admin" && storage::load_pin() == "0000");
    cJSON_AddBoolToObject(root, "using_default_creds", using_defaults);
    cJSON_AddStringToObject(root, "active_scene", g_scene_playing ? g_active_scene_name.c_str() : "");
    cJSON *color_obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(color_obj, "r", color[0]);
    cJSON_AddNumberToObject(color_obj, "g", color[1]);
    cJSON_AddNumberToObject(color_obj, "b", color[2]);
    cJSON_AddItemToObject(root, "color", color_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t history_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    cJSON *arr = cJSON_CreateArray();
    for (const auto &line : g_history) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(line.c_str()));
    }
    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

// Sube un firmware nuevo (.bin) por POST, cuerpo binario crudo (Content-Type: application/octet-stream)
esp_err_t ota_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fallo: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[1024];
    int received;
    int remaining = req->content_len;
    ESP_LOGI(TAG, "Recibiendo OTA: %d bytes", remaining);

    while (remaining > 0) {
        received = httpd_req_recv(req, buf, std::min((int)sizeof(buf), remaining));
        if (received <= 0) {
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        esp_ota_write(ota_handle, buf, received);
        remaining -= received;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end fallo: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition fallo: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OTA OK, reiniciando...", HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "OTA completo, reiniciando");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Configuración: cambiar/olvidar WiFi, apagar (deep sleep)
// ---------------------------------------------------------------------------
const char *WIFI_FORM_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Cambiar WiFi</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2rem}
input{width:90%;max-width:280px;padding:.6rem;margin:.5rem;border-radius:6px;border:none}
button{padding:.7rem 2rem;background:#2980b9;color:#fff;border:none;border-radius:8px;font-weight:bold}
</style></head><body>
<h2>Cambiar / agregar red WiFi</h2>
<p style="color:#999;font-size:.85rem">Al guardar, la placa se reinicia y se conecta con la red nueva.</p>
<form action="/wifi/save" method="POST">
<input name="ssid" placeholder="Nombre de red (SSID)" required><br>
<input name="pass" placeholder="Contraseña" type="password"><br>
<button type="submit">Guardar y conectar</button>
</form>
</body></html>
)HTML";

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

// Escapa caracteres peligrosos antes de insertar texto de usuario en HTML generado
// en el servidor (evita XSS almacenado, ej. a través de nombres de escena).
std::string htmlEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

esp_err_t wifi_form_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WIFI_FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wifi_save_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[256] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char ssid_raw[64] = {0}, pass_raw[128] = {0};
    httpd_query_key_value(buf, "ssid", ssid_raw, sizeof(ssid_raw));
    httpd_query_key_value(buf, "pass", pass_raw, sizeof(pass_raw));

    storage::save_wifi_credentials(urlDecode(ssid_raw), urlDecode(pass_raw));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h3>Guardado. Reiniciando...</h3>", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

esp_err_t wifi_forget_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    storage::clear_wifi_credentials();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK, reiniciando...", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

esp_err_t powersave_on_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    g_effects->stop();
    g_scene_playing = false; g_active_scene_name.clear();
    g_led->off();
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM); // el radio WiFi solo se despierta en cada intervalo DTIM
    g_power_save = true;
    g_history.clear(); // se borra el historial al entrar en modo ahorro
    ESP_LOGI(TAG, "Modo ahorro activado (WIFI_PS_MAX_MODEM)");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

esp_err_t powersave_off_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    esp_wifi_set_ps(WIFI_PS_NONE); // vuelve a respuesta inmediata, mayor consumo
    g_power_save = false;
    addHistory("Modo ahorro desactivado");
    ESP_LOGI(TAG, "Modo ahorro desactivado");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Escenas guardadas (secuencias de colores)
// ---------------------------------------------------------------------------
esp_err_t scenes_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    auto scenes = storage::load_scenes();
    cJSON *arr = cJSON_CreateArray();
    for (const auto &s : scenes) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", s.name);
        cJSON *colors = cJSON_CreateArray();
        for (int i = 0; i < s.step_count; i++) {
            cJSON *step = cJSON_CreateArray();
            cJSON_AddItemToArray(step, cJSON_CreateNumber(s.colors[i][0]));
            cJSON_AddItemToArray(step, cJSON_CreateNumber(s.colors[i][1]));
            cJSON_AddItemToArray(step, cJSON_CreateNumber(s.colors[i][2]));
            cJSON_AddItemToArray(colors, step);
        }
        cJSON_AddItemToObject(obj, "colors", colors);
        cJSON_AddItemToArray(arr, obj);
    }
    char *json_str = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(arr);
    return ESP_OK;
}

esp_err_t scenes_save_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[512] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    cJSON *name_item = cJSON_GetObjectItem(root, "name");
    cJSON *colors_item = cJSON_GetObjectItem(root, "colors");
    if (!cJSON_IsString(name_item) || !cJSON_IsArray(colors_item)) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    storage::Scene scene = {};
    strncpy(scene.name, name_item->valuestring, sizeof(scene.name) - 1);

    int n = cJSON_GetArraySize(colors_item);
    if (n > (int)storage::kMaxSceneSteps) n = storage::kMaxSceneSteps;
    scene.step_count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        cJSON *step = cJSON_GetArrayItem(colors_item, i);
        cJSON *rr = cJSON_GetArrayItem(step, 0);
        cJSON *gg = cJSON_GetArrayItem(step, 1);
        cJSON *bb = cJSON_GetArrayItem(step, 2);
        scene.colors[i][0] = (uint8_t)(rr ? rr->valueint : 0);
        scene.colors[i][1] = (uint8_t)(gg ? gg->valueint : 0);
        scene.colors[i][2] = (uint8_t)(bb ? bb->valueint : 0);
    }
    cJSON_Delete(root);

    bool ok = storage::save_scene(scene);
    addHistory(ok ? (std::string("Escena guardada: ") + scene.name) : "Error: sin espacio para mas escenas");

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, ok ? "OK" : "SIN ESPACIO", HTTPD_RESP_USE_STRLEN);
}

esp_err_t scenes_delete_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[64];
    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len > 1 && qs_len <= sizeof(buf) && httpd_req_get_url_query_str(req, buf, qs_len) == ESP_OK) {
        char name_raw[32] = {0};
        httpd_query_key_value(buf, "name", name_raw, sizeof(name_raw));
        std::string name = urlDecode(name_raw);
        storage::delete_scene(name);
        if (g_active_scene_name == name) { g_scene_playing = false; g_active_scene_name.clear(); }
        addHistory(std::string("Escena borrada: ") + name);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

esp_err_t scenes_play_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    if (g_power_save) {
        httpd_resp_set_status(req, "423 Locked");
        return httpd_resp_send(req, "En modo ahorro de energia", HTTPD_RESP_USE_STRLEN);
    }
    char buf[64];
    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len <= 1 || qs_len > sizeof(buf) || httpd_req_get_url_query_str(req, buf, qs_len) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char name_raw[32] = {0};
    httpd_query_key_value(buf, "name", name_raw, sizeof(name_raw));
    std::string name = urlDecode(name_raw);

    apply_scene_by_name(name);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

void scenePlayerTaskFn(void *) {
    size_t idx = 0;
    while (true) {
        if (g_scene_playing && !g_scene_colors.empty()) {
            const auto &c = g_scene_colors[idx % g_scene_colors.size()];
            g_led->setColor(c[0], c[1], c[2]);
            idx++;
            vTaskDelay(pdMS_TO_TICKS(700));
        } else {
            idx = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

}  // namespace

// Función compartida (declarada en WebServer.hpp) para que el Scheduler
// también pueda disparar una escena por nombre, igual que el dashboard.
void apply_scene_by_name(const std::string &name) {
    if (g_power_save) return;
    auto scenes = storage::load_scenes();
    for (const auto &s : scenes) {
        if (name == std::string(s.name) && s.step_count > 0) {
            g_effects->stop();
            g_scene_colors.clear();
            for (int i = 0; i < s.step_count; i++) {
                g_scene_colors.push_back({s.colors[i][0], s.colors[i][1], s.colors[i][2]});
            }
            g_active_scene_name = name;
            g_scene_playing = true;
            addHistory(std::string("Reproduciendo escena: ") + name);
            return;
        }
    }
}

namespace {

// ---------------------------------------------------------------------------
// Horarios programados
// ---------------------------------------------------------------------------
const char *SCHEDULE_PAGE_TEMPLATE = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Horarios</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;padding:1.5rem;font-size:14px}
h2{font-size:1.1rem} table{width:100%;border-collapse:collapse;margin-bottom:1rem}
td,th{padding:.4rem;border-bottom:1px solid #333;font-size:.8rem;text-align:left}
input,select{padding:.5rem;border-radius:4px;border:none;margin:.3rem .3rem .3rem 0}
button{padding:.5rem 1rem;background:#2980b9;color:#fff;border:none;border-radius:6px;font-weight:bold}
.del{background:#c0392b}
a{color:#2980b9}
label{display:block;font-size:.75rem;color:#999;margin-top:.6rem}
.field{display:none}
.field.show{display:inline-block}
</style></head><body>
<h2>Horarios programados</h2>
<p style="color:#999;font-size:.75rem">Hora del ESP32 (sincronizada por internet): %s</p>
<table><tr><th>Desde</th><th>Hasta</th><th>Acción</th><th>Detalle</th><th></th></tr>
%s
</table>
<h3>Agregar horario</h3>
<form action="/schedule/save" method="POST" id="sched-form">
<label>Desde</label>
<input type="time" name="start_time" required>
<label>Hasta (opcional — si lo pones, se apaga sola a esa hora)</label>
<input type="time" name="end_time">

<label>Qué hacer</label>
<select name="action" id="action-select">
<option value="0">Color fijo</option>
<option value="1">Efecto</option>
<option value="2">Escena guardada</option>
</select>

<div class="field" id="field-color">
  <label>Color</label>
  <input type="color" name="color_picker" id="color-picker" value="#ffffff">
</div>
<div class="field" id="field-effect">
  <label>Efecto</label>
  <select name="effect">
    <option value="1">Arcoíris</option>
    <option value="2">Respiración</option>
    <option value="3">Estroboscopio</option>
  </select>
</div>
<div class="field" id="field-scene">
  <label>Escena</label>
  <select name="scene_name" id="scene-select"></select>
</div>

<input type="hidden" name="color" id="color-hidden" value="255,255,255">
<br><button type="submit">Agregar</button>
</form>
<p><a href="/">&larr; Volver al dashboard</a></p>
<script>
const actionSelect = document.getElementById('action-select');
const fields = { 0: 'field-color', 1: 'field-effect', 2: 'field-scene' };
function updateFields() {
  Object.values(fields).forEach(id => document.getElementById(id).classList.remove('show'));
  document.getElementById(fields[actionSelect.value]).classList.add('show');
}
actionSelect.addEventListener('change', updateFields);
updateFields();

document.getElementById('color-picker').addEventListener('input', (e) => {
  const hex = e.target.value;
  const r = parseInt(hex.substr(1,2),16), g = parseInt(hex.substr(3,2),16), b = parseInt(hex.substr(5,2),16);
  document.getElementById('color-hidden').value = `${r},${g},${b}`;
});

fetch('/scenes').then(r => r.json()).then(scenes => {
  const sel = document.getElementById('scene-select');
  scenes.forEach(sc => {
    const opt = document.createElement('option');
    opt.value = sc.name; opt.textContent = sc.name;
    sel.appendChild(opt);
  });
});

function borrar(sh,sm){fetch(`/schedule/delete?start_hour=${sh}&start_minute=${sm}`,{method:'POST'}).then(()=>location.reload());}
</script>
</body></html>
)HTML";

esp_err_t schedule_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    auto schedules = storage::load_schedules();
    const char *action_names[] = {"Color", "Efecto", "Escena"};

    std::string rows;
    for (const auto &s : schedules) {
        char row[512];
        std::string detail;
        if (s.action == 0) {
            char buf2[32];
            snprintf(buf2, sizeof(buf2), "RGB(%d,%d,%d)", s.r, s.g, s.b);
            detail = buf2;
        } else if (s.action == 1) {
            char buf2[24];
            snprintf(buf2, sizeof(buf2), "efecto #%d", s.effect);
            detail = buf2;
        } else {
            detail = htmlEscape(s.scene_name); // nombre de escena: dato de usuario, se escapa
        }

        char end_buf[8];
        if (s.has_end) snprintf(end_buf, sizeof(end_buf), "%02d:%02d", s.end_hour, s.end_minute);
        else snprintf(end_buf, sizeof(end_buf), "-");

        snprintf(row, sizeof(row),
                 "<tr><td>%02d:%02d</td><td>%s</td><td>%s</td><td>%s</td>"
                 "<td><button class=\"del\" onclick=\"borrar(%d,%d)\">X</button></td></tr>",
                 s.start_hour, s.start_minute, end_buf, action_names[s.action], detail.c_str(),
                 s.start_hour, s.start_minute);
        rows += row;
    }
    if (rows.empty()) rows = "<tr><td colspan=5>Sin horarios configurados</td></tr>";

    char time_buf[32] = "no sincronizada aun";
    if (scheduler::is_time_synced()) {
        time_t now; time(&now);
        struct tm ti; localtime_r(&now, &ti);
        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &ti);
    }

    std::string page(SCHEDULE_PAGE_TEMPLATE);
    size_t pos = page.find("%s");
    page.replace(pos, 2, time_buf);
    pos = page.find("%s");
    page.replace(pos, 2, rows);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t schedule_save_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[512] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char start_raw[8] = {0}, end_raw[8] = {0}, action_raw[4] = {0},
         color_raw[32] = {0}, effect_raw[4] = {0}, scene_raw[32] = {0};
    httpd_query_key_value(buf, "start_time", start_raw, sizeof(start_raw));
    httpd_query_key_value(buf, "end_time", end_raw, sizeof(end_raw));
    httpd_query_key_value(buf, "action", action_raw, sizeof(action_raw));
    httpd_query_key_value(buf, "color", color_raw, sizeof(color_raw));
    httpd_query_key_value(buf, "effect", effect_raw, sizeof(effect_raw));
    httpd_query_key_value(buf, "scene_name", scene_raw, sizeof(scene_raw));

    std::string start_str = urlDecode(start_raw);
    std::string end_str = urlDecode(end_raw);
    std::string color_str = urlDecode(color_raw);
    std::string scene_str = urlDecode(scene_raw);

    storage::Schedule sched = {};
    sscanf(start_str.c_str(), "%hhu:%hhu", &sched.start_hour, &sched.start_minute);

    sched.has_end = !end_str.empty();
    if (sched.has_end) {
        sscanf(end_str.c_str(), "%hhu:%hhu", &sched.end_hour, &sched.end_minute);
    }

    sched.action = (uint8_t)atoi(action_raw);
    sched.effect = (uint8_t)atoi(effect_raw);
    int r = 255, g = 255, bl = 255;
    sscanf(color_str.c_str(), "%d,%d,%d", &r, &g, &bl);
    sched.r = (uint8_t)r; sched.g = (uint8_t)g; sched.b = (uint8_t)bl;
    strncpy(sched.scene_name, scene_str.c_str(), sizeof(sched.scene_name) - 1);
    sched.enabled = true;

    auto schedules = storage::load_schedules();
    if (schedules.size() < storage::kMaxSchedules) {
        schedules.push_back(sched);
        storage::save_schedules(schedules);
        addHistory("Horario agregado");
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/schedule");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t schedule_delete_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[64];
    size_t qs_len = httpd_req_get_url_query_len(req) + 1;
    if (qs_len > 1 && qs_len <= sizeof(buf) && httpd_req_get_url_query_str(req, buf, qs_len) == ESP_OK) {
        int start_hour = get_query_param_int(buf, "start_hour", -1);
        int start_minute = get_query_param_int(buf, "start_minute", -1);
        auto schedules = storage::load_schedules();
        schedules.erase(std::remove_if(schedules.begin(), schedules.end(), [&](const storage::Schedule &s) {
            return s.start_hour == start_hour && s.start_minute == start_minute;
        }), schedules.end());
        storage::save_schedules(schedules);
        addHistory("Horario borrado");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Cambio de usuario y PIN (exige el usuario y PIN anteriores)
// ---------------------------------------------------------------------------
const char *PIN_FORM_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Cambiar usuario y PIN</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2rem}
input{width:90%;max-width:220px;padding:.6rem;margin:.4rem;border-radius:6px;border:none;text-align:center}
button{padding:.7rem 2rem;background:#2980b9;color:#fff;border:none;border-radius:8px;font-weight:bold;margin-top:.6rem}
h3{font-size:.85rem;color:#999;margin:1.2rem 0 .3rem}
.err{color:#e74c3c;font-size:.8rem}
</style></head><body>
<h2>Cambiar usuario y PIN</h2>
<p style="color:#999;font-size:.8rem">Usuario actual: <b>%s</b></p>
<form action="/pin/change" method="POST">
<h3>Credenciales actuales</h3>
<input name="old_username" placeholder="Usuario actual" required>
<input name="old_pin" placeholder="PIN actual" type="password" autocomplete="current-password" required>
<h3>Credenciales nuevas</h3>
<input name="new_username" placeholder="Usuario nuevo" required>
<input name="new_pin" placeholder="PIN nuevo (4-20 caracteres, letras y números)" type="password" minlength="4" maxlength="20" autocomplete="new-password" required>
<input name="new_pin_confirm" placeholder="Confirma el PIN nuevo" type="password" minlength="4" maxlength="20" autocomplete="new-password" required>
<br><button type="submit">Cambiar</button>
</form>
<p><a href="/" style="color:#2980b9">&larr; Volver</a></p>
<script>
document.querySelector('form').addEventListener('submit', (e) => {
  const p1 = document.querySelector('[name=new_pin]').value;
  const p2 = document.querySelector('[name=new_pin_confirm]').value;
  if (p1 !== p2) {
    e.preventDefault();
    alert('El PIN nuevo y su confirmación no coinciden.');
  }
});
</script>
</body></html>
)HTML";

esp_err_t pin_form_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    std::string page(PIN_FORM_HTML);
    size_t pos = page.find("%s");
    page.replace(pos, 2, storage::load_username());
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t pin_change_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[224] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char old_username_raw[24] = {0}, old_pin_raw[32] = {0}, new_username_raw[24] = {0},
         new_pin_raw[32] = {0}, new_pin_confirm_raw[32] = {0};
    httpd_query_key_value(buf, "old_username", old_username_raw, sizeof(old_username_raw));
    httpd_query_key_value(buf, "old_pin", old_pin_raw, sizeof(old_pin_raw));
    httpd_query_key_value(buf, "new_username", new_username_raw, sizeof(new_username_raw));
    httpd_query_key_value(buf, "new_pin", new_pin_raw, sizeof(new_pin_raw));
    httpd_query_key_value(buf, "new_pin_confirm", new_pin_confirm_raw, sizeof(new_pin_confirm_raw));

    std::string old_username = urlDecode(old_username_raw);
    std::string old_pin = urlDecode(old_pin_raw);
    std::string new_username = urlDecode(new_username_raw);
    std::string new_pin_str = urlDecode(new_pin_raw);
    std::string new_pin_confirm = urlDecode(new_pin_confirm_raw);

    if (old_username != storage::load_username() || old_pin != storage::load_pin()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h3>Usuario o PIN actual incorrecto. <a href='/pin'>Volver</a></h3>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (new_pin_str != new_pin_confirm) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h3>El PIN nuevo y su confirmación no coinciden. <a href='/pin'>Volver</a></h3>",
                         HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    bool valid_pin = new_pin_str.size() >= 4 && new_pin_str.size() <= 20 &&
                      std::all_of(new_pin_str.begin(), new_pin_str.end(), ::isalnum);
    if (!valid_pin || new_username.empty()) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h3>El PIN nuevo debe tener entre 4 y 20 caracteres (letras y/o números), "
                              "y el usuario no puede estar vacío. "
                              "<a href='/pin'>Volver</a></h3>", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    storage::save_username(new_username);
    storage::save_pin(new_pin_str);
    addHistory("Usuario y PIN de acceso cambiados");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "<h3>Credenciales actualizadas. <a href='/'>Volver al dashboard</a></h3>", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Cambio de la contraseña de la red directa "ESP32-LED-Direct"
// ---------------------------------------------------------------------------
const char *AP_PASS_FORM_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Clave de la red directa</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:2rem}
input{width:90%;max-width:240px;padding:.6rem;margin:.4rem;border-radius:6px;border:none;text-align:center}
button{padding:.7rem 2rem;background:#2980b9;color:#fff;border:none;border-radius:8px;font-weight:bold;margin-top:.6rem}
</style></head><body>
<h2>Clave de la red "ESP32-LED-Direct"</h2>
<p style="color:#999;font-size:.8rem">Mínimo 8 caracteres (requisito de WPA2). Se aplica al instante, sin reiniciar.</p>
<form action="/ap-password/change" method="POST">
<input name="new_password" placeholder="Nueva clave (mín. 8 caracteres)" minlength="8" required>
<br><button type="submit">Cambiar</button>
</form>
<p><a href="/" style="color:#2980b9">&larr; Volver</a></p>
</body></html>
)HTML";

esp_err_t ap_pass_form_get_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, AP_PASS_FORM_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t ap_pass_change_post_handler(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char buf[96] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char new_pass_raw[80] = {0};
    httpd_query_key_value(buf, "new_password", new_pass_raw, sizeof(new_pass_raw));
    std::string new_pass = urlDecode(new_pass_raw);

    if (new_pass.size() < 8) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, "<h3>La clave debe tener al menos 8 caracteres. <a href='/ap-password'>Volver</a></h3>",
                         HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    storage::save_ap_password(new_pass);

    // Aplica la nueva clave de inmediato, sin reiniciar la placa.
    wifi_config_t ap_config = {};
    strcpy((char *)ap_config.ap.ssid, "ESP32-LED-Direct");
    ap_config.ap.ssid_len = strlen("ESP32-LED-Direct");
    strncpy((char *)ap_config.ap.password, new_pass.c_str(), sizeof(ap_config.ap.password) - 1);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);

    addHistory("Clave de la red directa cambiada");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "<h3>Clave actualizada. Los dispositivos conectados a 'ESP32-LED-Direct' "
                                 "van a tener que reconectarse con la clave nueva. "
                                 "<a href='/'>Volver al dashboard</a></h3>", HTTPD_RESP_USE_STRLEN);
}

// ---------------------------------------------------------------------------
// Login / Logout
// ---------------------------------------------------------------------------
const char *LOGIN_HTML = R"HTML(
<!DOCTYPE html><html lang="es"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Iniciar sesión - LED RGB ESP32-S3</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;text-align:center;padding:3rem 2rem}
h2{margin-bottom:1.5rem}
input{width:90%;max-width:240px;padding:.7rem;margin:.4rem;border-radius:6px;border:none;text-align:center}
button{padding:.7rem 2rem;background:#2980b9;color:#fff;border:none;border-radius:8px;font-weight:bold;margin-top:.8rem}
.err{color:#e74c3c;font-size:.85rem;margin-top:.6rem}
</style></head><body>
<h2>LED RGB - ESP32-S3</h2>
%s
<form action="/login" method="POST">
<input name="username" placeholder="Usuario" autocomplete="username" required><br>
<input name="pin" placeholder="PIN" type="password" autocomplete="current-password" required><br>
<button type="submit">Entrar</button>
</form>
</body></html>
)HTML";

esp_err_t login_get_handler(httpd_req_t *req) {
    // Si ya tiene sesión válida, no hace falta mostrar el login de nuevo.
    if (checkSession(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, nullptr, 0);
    }

    bool show_error = false;
    char query[32] = {0};
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[4];
        if (httpd_query_key_value(query, "error", val, sizeof(val)) == ESP_OK) show_error = true;
    }

    std::string page(LOGIN_HTML);
    size_t pos = page.find("%s");
    page.replace(pos, 2, show_error ? "<p class='err'>Usuario o PIN incorrecto (o demasiados intentos: espera un minuto).</p>" : "");

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t login_post_handler(httpd_req_t *req) {
    char buf[128] = {0};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;

    char user_raw[24] = {0}, pin_raw[16] = {0};
    httpd_query_key_value(buf, "username", user_raw, sizeof(user_raw));
    httpd_query_key_value(buf, "pin", pin_raw, sizeof(pin_raw));

    if (checkCredentials(urlDecode(user_raw), urlDecode(pin_raw))) {
        std::string token = generateSessionToken();
        createSession(token);

        char cookie_hdr[96];
        snprintf(cookie_hdr, sizeof(cookie_hdr),
                 "session=%s; HttpOnly; Path=/; Max-Age=86400; SameSite=Strict", token.c_str());
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_hdr);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        addHistory("Inicio de sesion exitoso");
        return httpd_resp_send(req, nullptr, 0);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login?error=1");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t logout_post_handler(httpd_req_t *req) {
    std::string token = getCookieValue(req, "session");
    if (!token.empty()) destroySessionToken(token);

    httpd_resp_set_hdr(req, "Set-Cookie", "session=; HttpOnly; Path=/; Max-Age=0; SameSite=Strict");
    addHistory("Sesion cerrada");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

}  // namespace

void start_main_webserver(Ws2812Led &led, EffectsManager &effects, const std::string &ip) {
    g_led = &led;
    g_effects = &effects;
    g_ip_str = ip;
    addHistory("Placa iniciada, conectada a la red");

    httpd_handle_t server = nullptr;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30;
    config.stack_size = 8192; // el handler de OTA necesita más pila

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor HTTP");
        return;
    }

    httpd_uri_t root_uri   = { "/", HTTP_GET, root_get_handler, nullptr };
    httpd_uri_t led_uri    = { "/led", HTTP_GET, led_get_handler, nullptr };
    httpd_uri_t effect_uri = { "/effect", HTTP_GET, effect_get_handler, nullptr };
    httpd_uri_t status_uri = { "/status", HTTP_GET, status_get_handler, nullptr };
    httpd_uri_t history_uri = { "/history", HTTP_GET, history_get_handler, nullptr };
    httpd_uri_t ota_uri    = { "/ota", HTTP_POST, ota_post_handler, nullptr };
    httpd_uri_t wifi_form_uri   = { "/wifi", HTTP_GET, wifi_form_get_handler, nullptr };
    httpd_uri_t wifi_save_uri   = { "/wifi/save", HTTP_POST, wifi_save_post_handler, nullptr };
    httpd_uri_t wifi_forget_uri = { "/wifi/forget", HTTP_POST, wifi_forget_post_handler, nullptr };
    httpd_uri_t ps_on_uri  = { "/powersave/on", HTTP_POST, powersave_on_post_handler, nullptr };
    httpd_uri_t ps_off_uri = { "/powersave/off", HTTP_POST, powersave_off_post_handler, nullptr };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &led_uri);
    httpd_register_uri_handler(server, &effect_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &history_uri);
    httpd_register_uri_handler(server, &ota_uri);
    httpd_register_uri_handler(server, &wifi_form_uri);
    httpd_register_uri_handler(server, &wifi_save_uri);
    httpd_register_uri_handler(server, &wifi_forget_uri);
    httpd_register_uri_handler(server, &ps_on_uri);
    httpd_register_uri_handler(server, &ps_off_uri);

    httpd_uri_t scenes_get_uri    = { "/scenes", HTTP_GET, scenes_get_handler, nullptr };
    httpd_uri_t scenes_save_uri   = { "/scenes/save", HTTP_POST, scenes_save_post_handler, nullptr };
    httpd_uri_t scenes_delete_uri = { "/scenes/delete", HTTP_POST, scenes_delete_post_handler, nullptr };
    httpd_uri_t scenes_play_uri   = { "/scenes/play", HTTP_POST, scenes_play_post_handler, nullptr };
    httpd_uri_t sched_get_uri     = { "/schedule", HTTP_GET, schedule_get_handler, nullptr };
    httpd_uri_t sched_save_uri    = { "/schedule/save", HTTP_POST, schedule_save_post_handler, nullptr };
    httpd_uri_t sched_delete_uri  = { "/schedule/delete", HTTP_POST, schedule_delete_post_handler, nullptr };
    httpd_uri_t pin_form_uri      = { "/pin", HTTP_GET, pin_form_get_handler, nullptr };
    httpd_uri_t pin_change_uri    = { "/pin/change", HTTP_POST, pin_change_post_handler, nullptr };
    httpd_uri_t ap_pass_form_uri   = { "/ap-password", HTTP_GET, ap_pass_form_get_handler, nullptr };
    httpd_uri_t ap_pass_change_uri = { "/ap-password/change", HTTP_POST, ap_pass_change_post_handler, nullptr };
    httpd_uri_t login_get_uri  = { "/login", HTTP_GET, login_get_handler, nullptr };
    httpd_uri_t login_post_uri = { "/login", HTTP_POST, login_post_handler, nullptr };
    httpd_uri_t logout_uri     = { "/logout", HTTP_POST, logout_post_handler, nullptr };

    httpd_register_uri_handler(server, &scenes_get_uri);
    httpd_register_uri_handler(server, &scenes_save_uri);
    httpd_register_uri_handler(server, &scenes_delete_uri);
    httpd_register_uri_handler(server, &scenes_play_uri);
    httpd_register_uri_handler(server, &sched_get_uri);
    httpd_register_uri_handler(server, &sched_save_uri);
    httpd_register_uri_handler(server, &sched_delete_uri);
    httpd_register_uri_handler(server, &pin_form_uri);
    httpd_register_uri_handler(server, &pin_change_uri);
    httpd_register_uri_handler(server, &ap_pass_form_uri);
    httpd_register_uri_handler(server, &ap_pass_change_uri);
    httpd_register_uri_handler(server, &login_get_uri);
    httpd_register_uri_handler(server, &login_post_uri);
    httpd_register_uri_handler(server, &logout_uri);

    xTaskCreate(scenePlayerTaskFn, "scene_player", 4096, nullptr, 4, nullptr);

    ESP_LOGI(TAG, "Servidor HTTP iniciado");
}
