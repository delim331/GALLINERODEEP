/*
  GALLINERO ULTRA – VERSIÓN 100% SIN REINICIOS
  D1 Mini + Relé portapilas + MX1552 + Sensor + Telegram + NTP + Ajustes totales
  - SIN REINICIOS AUTOMÁTICOS (ni por watchdog, ni por tiempo, ni por nada)
  - SIN BOTÓN DE REINICIO EN MENÚ
  - Todos los comandos accesibles desde un botón "📋 Comandos"
  - Ajustes completos: pulsos, tiempos mínimos, timeout, horarios
  - Estadísticas diarias
  - OTA solo si tú lo ordenas, y sin reinicio automático (se reinicia solo al finalizar la OTA, es inevitable, pero es bajo tu orden)
*/

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>

// ================= CONFIGURACIÓN =================
const char* WIFI_SSID = "Shore";
const char* WIFI_PASS = "00000000";
const char* BOT_TOKEN = "8578562809:AAHI71fTH_gACNfsxCZ1IXNgcpZ3SRzf__E";
const char* CHAT_ID = "690781778";

// ================= PINES =================
#define RELAY_PIN      D5
#define MX_OPEN_PIN    D6
#define MX_CLOSE_PIN   D7
#define SENSOR_PIN     D2

// ================= CONSTANTES =================
#define PULSE_OPEN_DEFAULT     500UL
#define PULSE_CLOSE_DEFAULT    500UL
#define MOTOR_MIN_TIME_DEFAULT 500UL
#define MOTOR_TIMEOUT_DEFAULT  15000UL
#define SENSOR_DEBOUNCE_MS     100UL
#define TELEGRAM_INTERVAL_MS   500UL
#define WIFI_TIMEOUT_MS        15000UL
#define EEPROM_MAGIC            0xAA

// ================= OBJETOS =================
WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

// ================= ENUMS =================
enum class DoorState : uint8_t { UNKNOWN, OPEN, CLOSED, OPENING, CLOSING };

// ================= CONFIGURACIÓN EEPROM =================
struct Config {
  bool autoMode = true;
  unsigned long pulseOpen = PULSE_OPEN_DEFAULT;
  unsigned long pulseClose = PULSE_CLOSE_DEFAULT;
  unsigned long motorMinTime = MOTOR_MIN_TIME_DEFAULT;
  unsigned long motorTimeout = MOTOR_TIMEOUT_DEFAULT;
  int onHour = 7;
  int onMinute = 0;
  int offHour = 20;
  int offMinute = 0;

  void load() {
    EEPROM.begin(512);
    if (EEPROM.read(0) == EEPROM_MAGIC) {
      autoMode = EEPROM.read(1);
      EEPROM.get(2, pulseOpen);
      EEPROM.get(6, pulseClose);
      EEPROM.get(10, motorMinTime);
      EEPROM.get(14, motorTimeout);
      onHour = EEPROM.read(18);
      onMinute = EEPROM.read(19);
      offHour = EEPROM.read(20);
      offMinute = EEPROM.read(21);
    }
    EEPROM.end();
  }

  void save() {
    EEPROM.begin(512);
    EEPROM.write(0, EEPROM_MAGIC);
    EEPROM.write(1, autoMode);
    EEPROM.put(2, pulseOpen);
    EEPROM.put(6, pulseClose);
    EEPROM.put(10, motorMinTime);
    EEPROM.put(14, motorTimeout);
    EEPROM.write(18, onHour);
    EEPROM.write(19, onMinute);
    EEPROM.write(20, offHour);
    EEPROM.write(21, offMinute);
    EEPROM.commit();
    EEPROM.end();
  }
};

Config config;

// ================= VARIABLES =================
DoorState doorState = DoorState::UNKNOWN;
bool motorActive = false;
unsigned long motorStartTime = 0;
unsigned long motorMinStartTime = 0;
bool motorMinMet = false;
unsigned long lastTelegramCheck = 0;

// Sensor
bool sensorStableState = false;
bool sensorLastRaw = false;
unsigned long lastSensorChange = 0;

// Control horario
int lastAutoMinute = -1;

// Estadísticas diarias
unsigned int dailyOpenCount = 0;
unsigned int dailyCloseCount = 0;
unsigned long lastDailyReport = 0;
const int reportHour = 21;
const int reportMinute = 0;

// ================= PROTOTIPOS =================
void initPins();
void ensureWiFi();
void handleTelegram();
void updateDoorState();
void checkMotorControl();
void autoControl();
void sendMenu();
void sendStatus();
void sendCommandsList();
void turnMotorOn();
void turnMotorOff();
void openDoor();
void closeDoor();
void pulsePin(uint8_t pin, unsigned long duration);
bool readSensorRaw();
bool readSensorDebounced();
void updateDailyCounts();
void sendDailyReport();
void setupOTA();

// ================= FUNCIONES AUXILIARES =================
void pulsePin(uint8_t pin, unsigned long duration) {
  digitalWrite(pin, HIGH);
  unsigned long start = millis();
  while (millis() - start < duration) {
    delay(1);
    yield();
  }
  digitalWrite(pin, LOW);
}

bool readSensorRaw() { return digitalRead(SENSOR_PIN) == LOW; }

bool readSensorDebounced() {
  bool current = readSensorRaw();
  if (current != sensorLastRaw) {
    lastSensorChange = millis();
    sensorLastRaw = current;
  }
  if (millis() - lastSensorChange > SENSOR_DEBOUNCE_MS) {
    sensorStableState = current;
  }
  return sensorStableState;
}

void turnMotorOn() {
  if (!motorActive) {
    digitalWrite(RELAY_PIN, HIGH);
    motorActive = true;
    motorStartTime = millis();
    motorMinStartTime = millis();
    motorMinMet = false;
    bot.sendMessage(CHAT_ID, "🔌 Motor encendido", "");
  }
}

void turnMotorOff() {
  if (motorActive) {
    digitalWrite(RELAY_PIN, LOW);
    motorActive = false;
    bot.sendMessage(CHAT_ID, "⛔ Motor apagado", "");
  }
}

void openDoor() {
  if (doorState == DoorState::OPEN || doorState == DoorState::OPENING) {
    bot.sendMessage(CHAT_ID, "La puerta ya está abierta o abriéndose", "");
    return;
  }
  turnMotorOn();
  pulsePin(MX_OPEN_PIN, config.pulseOpen);
  doorState = DoorState::OPENING;
  bot.sendMessage(CHAT_ID, "🚪 Abriendo puerta...", "");
}

void closeDoor() {
  if (doorState == DoorState::CLOSED || doorState == DoorState::CLOSING) {
    bot.sendMessage(CHAT_ID, "La puerta ya está cerrada o cerrándose", "");
    return;
  }
  turnMotorOn();
  pulsePin(MX_CLOSE_PIN, config.pulseClose);
  doorState = DoorState::CLOSING;
  bot.sendMessage(CHAT_ID, "🔒 Cerrando puerta...", "");
}

void checkMotorControl() {
  if (!motorActive) return;
  unsigned long ahora = millis();
  if (!motorMinMet && (ahora - motorMinStartTime >= config.motorMinTime)) motorMinMet = true;
  if (ahora - motorStartTime >= config.motorTimeout) {
    turnMotorOff();
    doorState = DoorState::UNKNOWN;
    bot.sendMessage(CHAT_ID, "⚠️ Tiempo máximo de motor alcanzado. Revise el sensor.", "");
  }
}

void updateDoorState() {
  bool closed = readSensorDebounced();
  if (doorState == DoorState::OPENING) {
    if (!closed) {
      doorState = DoorState::OPEN;
      if (motorMinMet) turnMotorOff();
      bot.sendMessage(CHAT_ID, "📢 Puerta abierta", "");
    }
  } else if (doorState == DoorState::CLOSING) {
    if (closed) {
      doorState = DoorState::CLOSED;
      if (motorMinMet) turnMotorOff();
      bot.sendMessage(CHAT_ID, "📢 Puerta cerrada", "");
    }
  } else {
    if (closed && doorState != DoorState::CLOSED) {
      doorState = DoorState::CLOSED;
      bot.sendMessage(CHAT_ID, "📢 Puerta cerrada (detección pasiva)", "");
    } else if (!closed && doorState != DoorState::OPEN) {
      doorState = DoorState::OPEN;
      bot.sendMessage(CHAT_ID, "📢 Puerta abierta (detección pasiva)", "");
    }
  }
}

// ================= AUTO HORARIO =================
void autoControl() {
  if (!config.autoMode) return;
  timeClient.update();
  int h = timeClient.getHours();
  int m = timeClient.getMinutes();
  if (m == lastAutoMinute) return;
  lastAutoMinute = m;
  if (h == config.onHour && m == config.onMinute) openDoor();
  else if (h == config.offHour && m == config.offMinute) closeDoor();
}

// ================= ESTADÍSTICAS =================
void updateDailyCounts() {
  static DoorState lastDoorState = DoorState::UNKNOWN;
  if (doorState != lastDoorState) {
    if (doorState == DoorState::OPEN) dailyOpenCount++;
    if (doorState == DoorState::CLOSED) dailyCloseCount++;
    lastDoorState = doorState;
  }
}

void sendDailyReport() {
  timeClient.update();
  int h = timeClient.getHours();
  int m = timeClient.getMinutes();
  if (h == reportHour && m == reportMinute && millis() - lastDailyReport > 60000) {
    String msg = "📊 *Resumen diario*\n";
    msg += "Puerta: " + String(doorState == DoorState::OPEN ? "ABIERTA ✅" : "CERRADA 🔒") + "\n";
    msg += "Aperturas hoy: " + String(dailyOpenCount) + "\n";
    msg += "Cierres hoy: " + String(dailyCloseCount) + "\n";
    bot.sendMessage(CHAT_ID, msg, "Markdown");
    dailyOpenCount = 0;
    dailyCloseCount = 0;
    lastDailyReport = millis();
  }
}

// ================= TELEGRAM =================
void sendMenu() {
  String kb = "[[\"🔌 Motor ON\",\"⛔ Motor OFF\"],"
              "[\"🚪 Abrir\",\"🔒 Cerrar\"],"
              "[\"📊 Estado\",\"⚙️ Ajustes\"],"
              "[\"📋 Comandos\",\"📲 OTA\"]]";  // <-- Botón con todos los comandos
  bot.sendMessageWithReplyKeyboard(CHAT_ID, "Panel de control (sin reinicios)", "", kb, true);
}

void sendStatus() {
  String estado = "📋 *Estado actual*\n";
  estado += "Puerta: ";
  switch (doorState) {
    case DoorState::OPEN:    estado += "ABIERTA ✅"; break;
    case DoorState::CLOSED:  estado += "CERRADA 🔒"; break;
    case DoorState::OPENING: estado += "ABRIENDO ⚙️"; break;
    case DoorState::CLOSING: estado += "CERRANDO ⚙️"; break;
    default:                 estado += "DESCONOCIDA";
  }
  estado += "\nMotor: " + String(motorActive ? "ON 🔌" : "OFF ⛔");
  estado += "\nModo auto: " + String(config.autoMode ? "ACTIVO ⏰" : "INACTIVO");
  estado += "\nApertura: " + String(config.onHour) + ":" + (config.onMinute < 10 ? "0" : "") + String(config.onMinute);
  estado += "\nCierre: " + String(config.offHour) + ":" + (config.offMinute < 10 ? "0" : "") + String(config.offMinute);
  estado += "\nPulso apertura: " + String(config.pulseOpen) + " ms";
  estado += "\nPulso cierre: " + String(config.pulseClose) + " ms";
  estado += "\nTiempo mínimo motor: " + String(config.motorMinTime) + " ms";
  estado += "\nTimeout motor: " + String(config.motorTimeout / 1000.0, 1) + " s";
  estado += "\nAperturas hoy: " + String(dailyOpenCount);
  estado += "\nCierres hoy: " + String(dailyCloseCount);
  estado += "\nHora NTP: " + timeClient.getFormattedTime();
  bot.sendMessage(CHAT_ID, estado, "Markdown");
}

void sendCommandsList() {
  String cmds = "📋 *Todos los comandos disponibles*\n\n";
  cmds += "🔌 Motor ON – Enciende motor\n";
  cmds += "⛔ Motor OFF – Apaga motor\n";
  cmds += "🚪 Abrir – Abre puerta\n";
  cmds += "🔒 Cerrar – Cierra puerta\n";
  cmds += "📊 Estado – Muestra estado\n";
  cmds += "⏰ Auto ON/OFF – Activa/desactiva automático\n";
  cmds += "🌅 /seton HH:MM – Hora apertura\n";
  cmds += "🌙 /setoff HH:MM – Hora cierre\n";
  cmds += "⚡ /pulse_open VALOR – Pulso apertura (ms)\n";
  cmds += "⚡ /pulse_close VALOR – Pulso cierre (ms)\n";
  cmds += "⏱️ /motor_min_time VALOR – Tiempo mínimo motor (ms)\n";
  cmds += "⏱️ /motor_timeout VALOR – Timeout motor (ms)\n";
  cmds += "📊 /reset_counters – Reinicia contadores diarios\n";
  cmds += "📲 /ota – Actualizar por URL (ej: /ota http://.../firmware.bin)\n";
  cmds += "🔄 Nota: No hay comando de reinicio. El sistema nunca se reinicia solo.";
  bot.sendMessage(CHAT_ID, cmds, "Markdown");
}

void handleTelegram() {
  int n = bot.getUpdates(bot.last_message_received + 1);
  for (int i = 0; i < n; i++) {
    if (String(bot.messages[i].chat_id) != CHAT_ID) continue;

    String txt = bot.messages[i].text;
    txt.trim(); // Eliminar espacios
    Serial.println("Comando: " + txt);

    // ===== COMANDOS PRINCIPALES =====
    if (txt == "/start") {
      sendMenu();
    }
    else if (txt == "🔌 Motor ON") {
      turnMotorOn();
    }
    else if (txt == "⛔ Motor OFF") {
      turnMotorOff();
    }
    else if (txt == "🚪 Abrir") {
      openDoor();
    }
    else if (txt == "🔒 Cerrar") {
      closeDoor();
    }
    else if (txt == "📊 Estado") {
      sendStatus();
    }
    else if (txt == "⚙️ Ajustes") {
      String kb = "[[\"⏰ Auto ON/OFF\"],"
                  "[\"🌅 Hora apertura\",\"🌙 Hora cierre\"],"
                  "[\"⚡ Pulsos apertura\",\"⚡ Pulsos cierre\"],"
                  "[\"⏱️ Tiempo mínimo\",\"⏱️ Timeout\"],"
                  "[\"🔙 Volver\"]]";
      bot.sendMessageWithReplyKeyboard(CHAT_ID, "Ajustes", "", kb, true);
    }
    else if (txt == "📋 Comandos") {
      sendCommandsList();
    }
    else if (txt == "📲 OTA" || txt == "/ota") {
      bot.sendMessage(CHAT_ID, "📤 *OTA*\n\nEnvía /ota seguido de la URL directa al archivo .bin\nEjemplo: /ota http://miservidor.com/firmware.bin", "Markdown");
    }
    else if (txt.startsWith("/ota ")) {
      String url = txt.substring(5);
      url.trim();
      if (url.startsWith("http") && (url.endsWith(".bin") || url.indexOf(".bin") > 0)) {
        bot.sendMessage(CHAT_ID, "🌐 Iniciando OTA. El sistema se reiniciará al finalizar (es normal).", "");
        delay(1000);
        WiFiClient clientOTA;
        ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
        ESPhttpUpdate.rebootOnUpdate(true); // El reinicio es inevitable aquí, pero solo ocurre si TÚ lo ordenas
        t_httpUpdate_return ret = ESPhttpUpdate.update(clientOTA, url);
        // Si llega aquí, la OTA falló
        bot.sendMessage(CHAT_ID, "❌ OTA fallida. Intenta de nuevo.", "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ URL no válida. Debe ser un enlace directo a .bin", "");
      }
    }
    else if (txt == "🔙 Volver") {
      sendMenu();
    }
    else if (txt == "⏰ Auto ON/OFF") {
      config.autoMode = !config.autoMode;
      config.save();
      bot.sendMessage(CHAT_ID, config.autoMode ? "⏰ Auto activado" : "⛔ Auto desactivado", "");
    }
    else if (txt == "🌅 Hora apertura") {
      bot.sendMessage(CHAT_ID, "Envía /seton HH:MM (ej: /seton 07:30)", "");
    }
    else if (txt == "🌙 Hora cierre") {
      bot.sendMessage(CHAT_ID, "Envía /setoff HH:MM (ej: /setoff 20:00)", "");
    }
    else if (txt == "⚡ Pulsos apertura") {
      bot.sendMessage(CHAT_ID, "Envía /pulse_open VALOR (ms, 100-3000)", "");
    }
    else if (txt == "⚡ Pulsos cierre") {
      bot.sendMessage(CHAT_ID, "Envía /pulse_close VALOR (ms, 100-3000)", "");
    }
    else if (txt == "⏱️ Tiempo mínimo") {
      bot.sendMessage(CHAT_ID, "Envía /motor_min_time VALOR (ms, 100-5000)", "");
    }
    else if (txt == "⏱️ Timeout") {
      bot.sendMessage(CHAT_ID, "Envía /motor_timeout VALOR (ms, 5000-30000)", "");
    }
    else if (txt.startsWith("/seton ")) {
      int h, m;
      if (sscanf(txt.c_str(), "/seton %d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        config.onHour = h;
        config.onMinute = m;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Hora apertura: " + String(h) + ":" + (m < 10 ? "0" : "") + String(m), "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Formato inválido. Usa /seton HH:MM", "");
      }
    }
    else if (txt.startsWith("/setoff ")) {
      int h, m;
      if (sscanf(txt.c_str(), "/setoff %d:%d", &h, &m) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60) {
        config.offHour = h;
        config.offMinute = m;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Hora cierre: " + String(h) + ":" + (m < 10 ? "0" : "") + String(m), "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Formato inválido. Usa /setoff HH:MM", "");
      }
    }
    else if (txt.startsWith("/pulse_open ")) {
      unsigned long val = txt.substring(12).toInt();
      if (val >= 100 && val <= 3000) {
        config.pulseOpen = val;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Pulso apertura: " + String(val) + " ms", "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Valor inválido (100-3000 ms)", "");
      }
    }
    else if (txt.startsWith("/pulse_close ")) {
      unsigned long val = txt.substring(13).toInt();
      if (val >= 100 && val <= 3000) {
        config.pulseClose = val;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Pulso cierre: " + String(val) + " ms", "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Valor inválido (100-3000 ms)", "");
      }
    }
    else if (txt.startsWith("/motor_min_time ")) {
      unsigned long val = txt.substring(16).toInt();
      if (val >= 100 && val <= 5000) {
        config.motorMinTime = val;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Tiempo mínimo motor: " + String(val) + " ms", "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Valor inválido (100-5000 ms)", "");
      }
    }
    else if (txt.startsWith("/motor_timeout ")) {
      unsigned long val = txt.substring(15).toInt();
      if (val >= 5000 && val <= 30000) {
        config.motorTimeout = val;
        config.save();
        bot.sendMessage(CHAT_ID, "✅ Timeout motor: " + String(val / 1000.0, 1) + " s", "");
      } else {
        bot.sendMessage(CHAT_ID, "❌ Valor inválido (5000-30000 ms)", "");
      }
    }
    else if (txt == "/reset_counters") {
      dailyOpenCount = 0;
      dailyCloseCount = 0;
      bot.sendMessage(CHAT_ID, "✅ Contadores diarios reiniciados", "");
    }
    else {
      bot.sendMessage(CHAT_ID, "❓ Comando no reconocido. Usa /start o consulta la lista en '📋 Comandos'", "");
    }
  }
}

// ================= WIFI =================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
    yield();
  }
}

// ================= OTA LOCAL (ArduinoOTA) =================
void setupOTA() {
  ArduinoOTA.setHostname("GallineroUltra");
  ArduinoOTA.onStart([]() { Serial.println("Inicio OTA local"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA local completado!"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA local: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error OTA local[%u]\n", error);
  });
  ArduinoOTA.begin();
}

// ================= INICIALIZACIÓN =================
void initPins() {
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW);
  pinMode(MX_OPEN_PIN, OUTPUT); digitalWrite(MX_OPEN_PIN, LOW);
  pinMode(MX_CLOSE_PIN, OUTPUT); digitalWrite(MX_CLOSE_PIN, LOW);
  pinMode(SENSOR_PIN, INPUT_PULLUP);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n🚀 GALLINERO ULTRA - SIN REINICIOS");

  initPins();
  config.load();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi conectado");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ No se pudo conectar WiFi");
  }

  client.setInsecure();
  timeClient.begin();

  sensorLastRaw = readSensorRaw();
  sensorStableState = sensorLastRaw;
  doorState = sensorStableState ? DoorState::CLOSED : DoorState::OPEN;

  setupOTA();

  if (WiFi.status() == WL_CONNECTED) {
    bot.sendMessage(CHAT_ID, "🚀 Sistema iniciado (sin reinicios)\nPuerta: " + String(doorState == DoorState::OPEN ? "ABIERTA" : "CERRADA"), "");
    sendMenu();
  }

  Serial.println("✅ Setup completado");
}

// ================= LOOP =================
void loop() {
  ensureWiFi();
  ArduinoOTA.handle();

  if (millis() - lastTelegramCheck > TELEGRAM_INTERVAL_MS) {
    if (WiFi.status() == WL_CONNECTED) handleTelegram();
    lastTelegramCheck = millis();
  }

  updateDoorState();
  checkMotorControl();
  autoControl();
  updateDailyCounts();
  sendDailyReport();

  delay(5);
}