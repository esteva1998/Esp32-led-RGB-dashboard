# LED RGB Dashboard — ESP32-S3 (ESP-IDF puro, C++)

Dashboard web para controlar el LED RGB (WS2812) integrado de una placa ESP32-S3
N16R8, escrito en **C++ sobre ESP-IDF puro** — sin el framework de Arduino.
Corre un servidor HTTP embebido en la propia placa que sirve un panel de
control completo: colores, efectos animados, escenas programables, horarios,
y administración remota, todo con autenticación por sesión.

> Proyecto hecho para aprender el ecosistema de ESP-IDF y C++ moderno en
> microcontroladores, sin depender de las abstracciones de Arduino.

## Índice

- [Características](#características)
- [Hardware necesario](#hardware-necesario)
- [Arquitectura](#arquitectura)
- [Estructura del proyecto](#estructura-del-proyecto)
- [Puesta en marcha](#puesta-en-marcha)
- [Uso del dashboard](#uso-del-dashboard)
- [Endpoints de la API](#endpoints-de-la-api)
- [Seguridad](#seguridad)
- [Limitaciones conocidas](#limitaciones-conocidas)
- [Licencia](#licencia)

## Características

**Control del LED**
- Sliders RGB + selector de color nativo, con vista previa en vivo.
- Paleta de 12 colores predefinidos de un toque.
- Efectos automáticos: arcoíris, respiración y estroboscopio (corren en una
  tarea de FreeRTOS aparte, sin bloquear el servidor web).
- **Escenas**: secuencias de colores personalizadas, creadas desde el propio
  dashboard, que se reproducen en bucle.
- **Horarios programados**: hora de inicio y fin (opcional) por evento, con
  hora sincronizada por internet (SNTP). Cada horario puede disparar un color
  fijo, un efecto o una escena guardada.

**Conectividad**
- Portal de configuración WiFi cautivo la primera vez que arranca (sin
  credenciales quemadas en el código) — conecta en vivo, sin reiniciar la
  placa, y muestra la IP nueva en la misma página.
- Modo **AP + STA simultáneo**: además de conectarse a tu red, mantiene una
  red directa propia (`ESP32-LED-Direct`) siempre disponible como respaldo.
- mDNS (`http://led-dashboard.local`) para no depender de memorizar la IP
  (soporte variable según sistema operativo/navegador).
- Botón "Olvidar WiFi" y "Cambiar WiFi" en vivo desde el dashboard.

**Administración**
- **Modo de ahorro de energía**: baja el consumo del radio WiFi y bloquea el
  dashboard (incluso a nivel de servidor, no solo visual) hasta reactivarlo.
- Actualización de firmware (OTA) subiendo un `.bin` desde el navegador, sin
  cable USB.
- Historial de eventos en vivo (se borra automáticamente al entrar en modo
  ahorro).
- Panel de estado: IP, red conectada, uptime, memoria libre, señal WiFi.

**Seguridad**
- Autenticación por **sesión con cookie** (`HttpOnly` + `SameSite=Strict`),
  no HTTP Basic Auth.
- Usuario y PIN configurables (alfanumérico, 4-20 caracteres), con
  confirmación al cambiarlo y exigencia del PIN anterior.
- Bloqueo temporal tras intentos fallidos repetidos (defensa contra fuerza
  bruta).
- Protección CSRF (verificación de origen de la petición).
- Contraseña de la red directa configurable desde NVS (no vive en el código
  fuente).
- Ver la sección [Seguridad](#seguridad) para más detalle y limitaciones
  conocidas.

## Hardware necesario

- Una placa **ESP32-S3 N16R8** (16MB flash, 8MB PSRAM) con LED RGB WS2812
  integrado en GPIO48 — este proyecto se desarrolló y probó sobre el perfil
  `4d_systems_esp32s3_gen4_r8n16` de PlatformIO, que coincide con las
  especificaciones de flash/PSRAM de una N16R8 genérica.
- Cable USB-C para flashear.

No se necesita ningún componente electrónico adicional — todo corre sobre el
LED y el hardware ya integrados en la placa.

## Arquitectura

```
┌─────────────┐     ┌──────────────┐     ┌────────────────┐
│ WifiManager │────▶│  Scheduler   │────▶│  EffectsManager │
│ (STA + AP)  │     │ (SNTP+horario)│     │ (tarea FreeRTOS)│
└─────────────┘     └──────────────┘     └────────────────┘
       │                     │                     │
       └──────────┬──────────┴──────────┬──────────┘
                   ▼                     ▼
            ┌─────────────┐      ┌─────────────┐
            │  WebServer  │◀────▶│  Ws2812Led  │
            │ (dashboard) │      │ (driver RMT)│
            └─────────────┘      └─────────────┘
                   │
                   ▼
            ┌─────────────┐
            │   Storage   │
            │  (NVS/flash)│
            └─────────────┘
```

- **`Ws2812Led`**: clase RAII que envuelve el periférico RMT nativo de
  ESP-IDF para manejar el WS2812 sin ningún componente externo (evita
  depender de `led_strip`, que da problemas con el gestor de componentes de
  PlatformIO).
- **`EffectsManager`**: efectos automáticos y reproducción de escenas, en su
  propia tarea de FreeRTOS.
- **`WifiManager`**: conexión STA con credenciales guardadas, o portal de
  configuración por AP si no las hay; mantiene una red directa permanente.
- **`Scheduler`**: sincroniza la hora por SNTP y dispara los horarios
  guardados.
- **`Storage`**: capa sobre NVS para credenciales, escenas, horarios, y
  configuración de seguridad.
- **`WebServer`**: el servidor HTTP, el dashboard, y toda la lógica de
  autenticación.

## Estructura del proyecto

```
.
├── platformio.ini          # Configuración de build (placa, framework, particiones)
├── partitions.csv          # Tabla de particiones custom (2 slots OTA)
└── src/
    ├── main.cpp             # Orquestación: arranca todos los módulos
    ├── Ws2812Led.hpp/.cpp    # Driver del LED (RMT)
    ├── Effects.hpp/.cpp      # Efectos automáticos y reproducción de escenas
    ├── WifiManager.hpp/.cpp  # WiFi STA/AP + portal de configuración
    ├── Scheduler.hpp/.cpp    # SNTP + horarios programados
    ├── Storage.hpp/.cpp      # Persistencia en NVS
    ├── WebServer.hpp/.cpp    # Servidor HTTP + dashboard + autenticación
    └── idf_component.yml    # Dependencia del componente mDNS
```

## Puesta en marcha

### Requisitos

- [PlatformIO](https://platformio.org/) (extensión de VS Code o CLI).
- Framework `espidf` (PlatformIO lo descarga solo al compilar la primera
  vez).

### Compilar y flashear

```bash
git clone https://github.com/esteva1998/Esp32-led-RGB-dashboard.git
cd https://github.com/esteva1998/Esp32-led-RGB-dashboard.git
pio run --target upload
```

La primera compilación tarda varios minutos (descarga el toolchain completo
de ESP-IDF). Compilaciones siguientes son mucho más rápidas.

### Primer arranque — configurar WiFi

La placa **no trae ninguna credencial de WiFi precargada**. En el primer
arranque (o después de "Olvidar WiFi"):

1. La placa crea su propia red **`ESP32-LED-Setup`** (sin contraseña).
2. Conéctate a esa red desde tu celular/PC.
3. Abre `http://192.168.4.1` en el navegador.
4. Ingresa el SSID y contraseña de tu red real.
5. La placa se conecta en vivo (sin reiniciar) y la misma página te muestra
   la IP nueva con un link directo al dashboard.

### Primer acceso al dashboard

Usuario y PIN por defecto:

```
Usuario: admin
PIN:     0000
```

**Cámbialos apenas puedas** desde el botón "Cambiar PIN" del dashboard — el
propio dashboard te lo recuerda con un aviso visible mientras sigan siendo
los valores por defecto.

## Uso del dashboard

- **Sliders / selector de color**: control manual, en tiempo real.
- **Colores rápidos**: paleta de un toque.
- **Escenas**: botón "＋ nueva" abre un constructor donde eliges varios
  colores en orden y les pones nombre; al tocar la escena, los reproduce en
  bucle.
- **Efectos**: arcoíris, respiración, estroboscopio — un toque para
  activar/desactivar.
- **Horario**: agrega horarios con hora de inicio y fin opcional, y qué
  aplicar (color, efecto o escena).
- **Config**: cambiar WiFi, olvidar WiFi, modo ahorro de energía, cambiar
  usuario/PIN, cambiar la clave de la red directa, cerrar sesión.

## Endpoints de la API

Todos (salvo `/login`) requieren una sesión válida (cookie `session`).

| Método | Ruta               | Descripción                                   |
|--------|--------------------|------------------------------------------------|
| GET    | `/`                | Dashboard principal                            |
| GET    | `/login`           | Página de inicio de sesión                     |
| POST   | `/login`           | Autentica y crea la sesión                     |
| POST   | `/logout`          | Cierra la sesión activa                        |
| GET    | `/led`             | Cambia el color (`?r=&g=&b=`)                  |
| GET    | `/effect`          | Activa/desactiva un efecto (`?id=`)            |
| GET    | `/scenes`          | Lista las escenas guardadas (JSON)             |
| POST   | `/scenes/save`     | Guarda una escena (JSON: `{name, colors}`)     |
| POST   | `/scenes/play`     | Reproduce una escena (`?name=`)                |
| POST   | `/scenes/delete`   | Borra una escena (`?name=`)                    |
| GET    | `/schedule`        | Página de horarios programados                 |
| POST   | `/schedule/save`   | Agrega un horario                              |
| POST   | `/schedule/delete` | Borra un horario                               |
| GET    | `/status`          | Estado en JSON (IP, red, uptime, RAM, etc.)    |
| GET    | `/history`         | Historial de eventos recientes (JSON)          |
| GET    | `/wifi`            | Formulario para cambiar de red WiFi            |
| POST   | `/wifi/save`       | Guarda una red WiFi nueva                      |
| POST   | `/wifi/forget`     | Olvida la red guardada (vuelve al portal)      |
| POST   | `/powersave/on`    | Activa el modo ahorro de energía               |
| POST   | `/powersave/off`   | Desactiva el modo ahorro de energía            |
| GET    | `/pin`             | Formulario para cambiar usuario/PIN            |
| POST   | `/pin/change`      | Cambia usuario/PIN (exige los actuales)        |
| GET    | `/ap-password`     | Formulario para cambiar la clave de la red directa |
| POST   | `/ap-password/change` | Cambia la clave de `ESP32-LED-Direct`       |
| POST   | `/ota`             | Sube un firmware nuevo (cuerpo binario)        |

## Seguridad

Este proyecto pasó por una revisión de seguridad deliberada — no es solo un
LED parpadeando, también es un ejercicio de exponer un dispositivo embebido
en una red de forma razonablemente responsable:

- **Sesiones, no Basic Auth**: token aleatorio de 128 bits (generador de
  hardware del ESP32), cookie `HttpOnly` + `SameSite=Strict`, expira a las
  24h.
- **Bloqueo por fuerza bruta**: 5 intentos fallidos → 60 segundos de espera.
- **CSRF**: se verifica el header `Sec-Fetch-Site` en cada petición
  autenticada.
- **Nada de credenciales en el código fuente**: ni el WiFi del usuario ni la
  clave de la red directa están hardcodeadas — todo vive en NVS,
  configurable desde el dashboard.

### Limitaciones conocidas (a propósito, con motivo)

- **Sin HTTPS**: se evaluó explícitamente y se descartó. Un certificado
  autofirmado (la única opción posible para un dispositivo en una IP local
  sin dominio público) muestra una advertencia de navegador *peor* que el
  HTTP plano actual, y consume bastante más RAM/CPU.
- **Sin Secure Boot / Flash Encryption**: técnicamente mitigarían el acceso
  físico a la flash, pero requieren quemar fusibles del chip de forma
  **irreversible** — fuera de alcance para un proyecto de este tipo.
- El portal de configuración inicial (`ESP32-LED-Setup`) es una red abierta
  por diseño (para que sea fácil de configurar) — es vulnerable a
  intercepción si alguien está escuchando exactamente durante esos segundos
  de setup.

## Licencia

MIT — ver [LICENSE](LICENSE).
por Jose Miguel Pastor Calabrese Aponte
Sientete libre de usar este proyecto y cualquier modificacion o mejora peudes hacerla y con gusto compartila para seguir apoyando el uso
de la electronica y la programacion a mas personas.