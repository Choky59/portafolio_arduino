// Proyecto #1: ¿Se puede salir?
// Sensor de temperatura DHT11 + ESP32 que reporta al portafolio cada 30 segundos.
// Entre lecturas duerme (deep sleep) para que las baterias duren mas.
//
// Conexiones (pines del lado izquierdo, lado del 3V3):
//   DHT11 VCC  -> 3V3
//   DHT11 DATA -> GPIO 26
//   DHT11 GND  -> GND
// Si tu DHT11 es el sensor suelto de 4 patas (sin modulo), agrega una
// resistencia de 10k entre VCC y DATA.
//
// Librerias (Gestor de librerias del Arduino IDE):
//   - "DHT sensor library" de Adafruit
//   - "Adafruit Unified Sensor"
//   - "ArduinoJson" de Benoit Blanchon (v7)
//
// Antes de compilar: copia secrets.example.h como secrets.h y llena tus datos.
// secrets.h no se sube al repo.
//
// Como se vincula (una sola vez):
//   1. En el panel /admin de la pagina crea el sensor y copia el codigo (ej. K7QM-4XRT).
//   2. Abre el Monitor Serie a 115200 baudios, escribe el codigo y presiona Enter.
//      Mientras no tenga llave, el ESP32 no se duerme: se queda esperando el codigo.
//   3. El ESP32 lo canjea por su llave secreta y la guarda en su memoria (NVS).
//      Desde ahi mide, envia y duerme solo, aunque lo desconectes y lo vuelvas a conectar.
//
// Ciclo normal: despierta -> lee el sensor -> conecta WiFi -> envia -> duerme hasta
// completar 30 s. Lo que mas energia gasta es el WiFi, asi que el canal y el router
// se guardan en la memoria RTC (sobrevive al deep sleep) para reconectar mas rapido.
//
// Comandos por Serial: al encender o presionar EN/RESET hay 5 segundos para escribir
// "estado" u "olvidar" (borra la llave para vincularlo de nuevo). Al despertar del
// sueno no se esperan comandos, para no gastar bateria.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <esp_sleep.h>

#include "secrets.h"

#define FIRMWARE_VERSION "1.1.0"

#define DHTPIN 26       // GPIO 26
#define DHTTYPE DHT11

const uint32_t INTERVALO_MS = 30000;            // una lectura cada 30 s, igual que el dashboard
const unsigned long VENTANA_COMANDOS_MS = 5000; // al encender, tiempo para "estado" / "olvidar"
const unsigned long WIFI_RAPIDO_MS = 5000;      // reconexion con canal y router guardados
const unsigned long WIFI_COMPLETO_MS = 15000;   // reconexion normal (busca el router)
const unsigned long REINTENTO_WIFI_MS = 10000;  // modo vinculacion: reintento si se cae

// Memoria RTC: sobrevive al deep sleep (se pierde al quitar la corriente)
RTC_DATA_ATTR int32_t wifiCanal = 0;
RTC_DATA_ATTR uint8_t wifiBssid[6];
RTC_DATA_ATTR uint32_t ciclos = 0;

DHT dht(DHTPIN, DHTTYPE);
Preferences memoria;
WiFiClientSecure clienteSeguro;
WiFiClient clientePlano;

String deviceId;
String deviceSecret;
String lineaSerial;
bool despertoDelSueno = false;
unsigned long ultimoIntentoWifi = 0;

/* -------------------------------------------------------------------------- */
/*                               Credenciales                                 */
/* -------------------------------------------------------------------------- */

bool estaVinculado() {
  return deviceId.length() > 0 && deviceSecret.length() > 0;
}

void cargarCredenciales() {
  memoria.begin("portafolio", true);
  deviceId = memoria.getString("deviceId", "");
  deviceSecret = memoria.getString("secret", "");
  memoria.end();
}

void guardarCredenciales(const String& id, const String& secret) {
  memoria.begin("portafolio", false);
  memoria.putString("deviceId", id);
  memoria.putString("secret", secret);
  memoria.end();
  deviceId = id;
  deviceSecret = secret;
}

void olvidarCredenciales() {
  memoria.begin("portafolio", false);
  memoria.clear();
  memoria.end();
  deviceId = "";
  deviceSecret = "";
}

void pedirCodigo() {
  Serial.println();
  Serial.println("Este sensor no esta vinculado (no se dormira hasta tener su llave).");
  Serial.println("Crea el sensor en el panel /admin, escribe aqui el codigo (ej. K7QM-4XRT) y presiona Enter.");
}

/* -------------------------------------------------------------------------- */
/*                                   WiFi                                     */
/* -------------------------------------------------------------------------- */

bool esperarWifi(unsigned long limiteMs) {
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < limiteMs) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Primero intenta con el canal y el router de la ultima vez (rapido); si falla, conexion normal.
bool conectarWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;  // e.g. right after linking
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);

  if (wifiCanal > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, wifiCanal, wifiBssid, true);
    if (esperarWifi(WIFI_RAPIDO_MS)) {
      ultimoIntentoWifi = millis();
      return true;
    }
    WiFi.disconnect();
    wifiCanal = 0;  // el router cambio de canal o no esta: buscarlo de nuevo
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  bool ok = esperarWifi(WIFI_COMPLETO_MS);
  if (ok) {
    wifiCanal = WiFi.channel();
    memcpy(wifiBssid, WiFi.BSSID(), 6);
  }
  ultimoIntentoWifi = millis();
  return ok;
}

// Solo en modo vinculacion (despierto): reintenta si se cae
bool wifiListo() {
  if (WiFi.status() == WL_CONNECTED) return true;
  if (millis() - ultimoIntentoWifi > REINTENTO_WIFI_MS) {
    Serial.println("WiFi desconectado, reintentando...");
    WiFi.reconnect();
    ultimoIntentoWifi = millis();
  }
  return false;
}

/* -------------------------------------------------------------------------- */
/*                                   HTTP                                     */
/* -------------------------------------------------------------------------- */

bool usaHttps() {
  return String(API_URL).startsWith("https://");
}

// POST con JSON. Devuelve el codigo HTTP (negativo si fallo la conexion).
int postJson(const char* ruta, const String& cuerpo, String& respuesta, bool conLlave) {
  HTTPClient http;
  String url = String(API_URL) + ruta;

  bool iniciado = usaHttps() ? http.begin(clienteSeguro, url) : http.begin(clientePlano, url);
  if (!iniciado) return -1;

  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  if (conLlave) {
    http.addHeader("Authorization", "Device " + deviceId + ":" + deviceSecret);
  }

  int codigo = http.POST(cuerpo);
  respuesta = codigo > 0 ? http.getString() : http.errorToString(codigo);
  http.end();
  return codigo;
}

// El servidor dice que la llave ya no sirve (sensor revocado o borrado en el panel)
void llaveRechazada() {
  Serial.println("El servidor rechazo la llave (sensor revocado o borrado en el panel).");
  olvidarCredenciales();
}

/* -------------------------------------------------------------------------- */
/*                                 Deep sleep                                 */
/* -------------------------------------------------------------------------- */

// Duerme lo que falte para completar el intervalo (el tiempo despierto ya cuenta)
void dormir() {
  uint32_t despierto = millis();
  uint32_t restante = despierto < INTERVALO_MS ? INTERVALO_MS - despierto : 1000;
  if (restante < 1000) restante = 1000;

  Serial.print("Despierto ");
  Serial.print(despierto);
  Serial.print(" ms. Durmiendo ");
  Serial.print(restante / 1000.0, 1);
  Serial.println(" s...");
  Serial.flush();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)restante * 1000ULL);
  esp_deep_sleep_start();
}

/* -------------------------------------------------------------------------- */
/*                          Vinculacion y telemetria                          */
/* -------------------------------------------------------------------------- */

// El DHT11 a veces falla una lectura: reintenta un par de veces
bool leerSensor(float& temperatura, float& humedad) {
  for (int intento = 0; intento < 3; intento++) {
    humedad = dht.readHumidity();
    temperatura = dht.readTemperature();  // Celsius
    if (!isnan(humedad) && !isnan(temperatura)) return true;
    delay(2000);  // el DHT11 necesita 1-2 s entre lecturas
  }
  return false;
}

void imprimirLectura(float temperatura, float humedad) {
  Serial.print("Humedad: ");
  Serial.print(humedad);
  Serial.print(" %  |  Temperatura: ");
  Serial.print(temperatura);
  Serial.print(" °C / ");
  Serial.print(dht.convertCtoF(temperatura));
  Serial.print(" °F  |  Sensacion termica: ");
  Serial.print(dht.computeHeatIndex(temperatura, humedad, false));
  Serial.println(" °C");
}

void enviarLectura(float temperatura, float humedad) {
  JsonDocument doc;
  doc["temperatura"] = temperatura;
  doc["humedad"] = humedad;
  String cuerpo;
  serializeJson(doc, cuerpo);

  String respuesta;
  int estado = postJson("/api/telemetria", cuerpo, respuesta, true);

  if (estado == 201) {
    Serial.println("  -> enviado a la pagina");
  } else if (estado == 401) {
    llaveRechazada();
  } else if (estado == 409) {
    Serial.println("  -> el sensor no tiene proyecto: asignale \"¿Se puede salir?\" en el panel /admin");
  } else {
    Serial.print("  -> no se pudo enviar (");
    Serial.print(estado);
    Serial.print("): ");
    Serial.println(respuesta);
  }
}

// Al encender (no al despertar): confirma la llave y reporta la version del firmware al panel
void verificarLlave() {
  JsonDocument doc;
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  String cuerpo;
  serializeJson(doc, cuerpo);

  String respuesta;
  int estado = postJson("/api/devices/heartbeat", cuerpo, respuesta, true);
  if (estado == 200) {
    Serial.print("Llave valida. Sensor ");
    Serial.println(deviceId);
  } else if (estado == 401) {
    llaveRechazada();
  }
}

/**
 * Un ciclo completo: lee el sensor (antes de prender el WiFi, que es lo que mas gasta),
 * conecta, envia y se duerme. Si la llave fue rechazada no duerme: espera un codigo nuevo.
 */
void cicloDeMedicion() {
  ciclos++;
  float temperatura, humedad;
  bool lecturaOk = leerSensor(temperatura, humedad);
  if (lecturaOk) imprimirLectura(temperatura, humedad);
  else Serial.println("Error al leer el DHT11. Revisa las conexiones.");

  if (!conectarWifi()) {
    Serial.println("Sin WiFi; se reintenta en el siguiente ciclo.");
    dormir();
  }

  if (!despertoDelSueno) verificarLlave();
  if (estaVinculado() && lecturaOk) enviarLectura(temperatura, humedad);

  if (!estaVinculado()) {
    pedirCodigo();
    return;  // loop() se queda esperando el codigo, sin dormir
  }
  dormir();
}

void vincular(String codigo) {
  codigo.trim();
  codigo.toUpperCase();
  if (codigo.length() < 8) {
    Serial.println("Ese codigo no parece valido. Debe verse como K7QM-4XRT.");
    return;
  }
  if (!wifiListo()) {
    Serial.println("Sin WiFi; intenta de nuevo en unos segundos.");
    return;
  }

  JsonDocument doc;
  doc["claimCode"] = codigo;
  doc["hardwareId"] = WiFi.macAddress();
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  String cuerpo;
  serializeJson(doc, cuerpo);

  Serial.println("Canjeando codigo...");
  String respuesta;
  int estado = postJson("/api/devices/claim", cuerpo, respuesta, false);

  if (estado == 200) {
    JsonDocument res;
    if (deserializeJson(res, respuesta) || !res["deviceId"].is<const char*>() || !res["deviceSecret"].is<const char*>()) {
      Serial.println("Respuesta inesperada del servidor.");
      return;
    }
    guardarCredenciales(res["deviceId"].as<String>(), res["deviceSecret"].as<String>());
    Serial.print("Vinculado como ");
    Serial.println(deviceId);
    Serial.println("La llave quedo guardada en la memoria del ESP32. Desde ahora mide, envia y duerme solo.");
    despertoDelSueno = true;  // la llave se acaba de validar: no hace falta el heartbeat
    cicloDeMedicion();
  } else if (estado == 401) {
    Serial.println("Codigo invalido, vencido o ya usado. Genera uno nuevo en el panel.");
  } else if (estado == 429) {
    Serial.println("Demasiados intentos. Espera unos minutos.");
  } else {
    Serial.print("No se pudo vincular (");
    Serial.print(estado);
    Serial.print("): ");
    Serial.println(respuesta);
  }
}

/* -------------------------------------------------------------------------- */
/*                              Comandos Serial                               */
/* -------------------------------------------------------------------------- */

void procesarComando(String linea) {
  linea.trim();
  if (linea.length() == 0) return;

  String comando = linea;
  comando.toLowerCase();

  if (comando == "estado") {
    Serial.print("Firmware: ");
    Serial.println(FIRMWARE_VERSION);
    Serial.print("Servidor: ");
    Serial.println(API_URL);
    Serial.print("Intervalo: ");
    Serial.print(INTERVALO_MS / 1000);
    Serial.println(" s (duerme entre lecturas)");
    Serial.print("Sensor: ");
    Serial.println(estaVinculado() ? deviceId : "sin vincular");
  } else if (comando == "olvidar") {
    olvidarCredenciales();
    Serial.println("Llave borrada.");
  } else if (!estaVinculado()) {
    vincular(linea);
  } else {
    Serial.println("Comandos: estado | olvidar. (Ya esta vinculado; usa \"olvidar\" para vincularlo de nuevo.)");
  }
}

// Devuelve true cuando se completa una linea (Enter)
bool leerSerial() {
  bool hubo = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineaSerial.length()) {
        procesarComando(lineaSerial);
        hubo = true;
      }
      lineaSerial = "";
    } else if (lineaSerial.length() < 64) {
      lineaSerial += c;
    }
  }
  return hubo;
}

// Solo al encender o con EN/RESET: unos segundos para "estado" / "olvidar" antes de dormir
void ventanaDeComandos() {
  Serial.print("Comandos durante ");
  Serial.print(VENTANA_COMANDOS_MS / 1000);
  Serial.println(" s: estado | olvidar");
  unsigned long inicio = millis();
  while (millis() - inicio < VENTANA_COMANDOS_MS && estaVinculado()) {
    leerSerial();
    delay(20);
  }
}

/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  despertoDelSueno = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  dht.begin();

  if (usaHttps()) {
    // Valida el certificado del servidor: sin esto alguien podria hacerse pasar por la API
    clienteSeguro.setCACert(ROOT_CA);
  }

  cargarCredenciales();

  if (!despertoDelSueno) {
    delay(500);
    Serial.println();
    Serial.println("¿Se puede salir? - sensor de temperatura v" FIRMWARE_VERSION);
    if (!usaHttps()) Serial.println("AVISO: API_URL usa http://. Solo para pruebas en tu red local.");
    if (estaVinculado()) ventanaDeComandos();
  }

  if (!estaVinculado()) {
    // Sin llave: se queda despierto con WiFi, esperando el codigo por Serial
    conectarWifi();
    pedirCodigo();
    return;
  }

  cicloDeMedicion();  // mide, envia y duerme (no regresa, salvo que la llave sea rechazada)
}

// Solo se llega aqui sin llave: modo vinculacion, despierto
void loop() {
  leerSerial();
  wifiListo();
  delay(20);
}
