// Proyecto #1: ¿Se puede salir?
// Sensor de temperatura DHT11 + ESP32 que reporta al portafolio cada 30 segundos.
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
//   3. El ESP32 lo canjea por su llave secreta y la guarda en su memoria (NVS).
//      Desde ahi reporta solo, aunque lo desconectes y lo vuelvas a conectar.
//
// Comandos por Serial: el codigo de vinculacion, "estado" o "olvidar" (borra la llave).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <DHT.h>

#include "secrets.h"

#define FIRMWARE_VERSION "1.0.0"

#define DHTPIN 26       // GPIO 26
#define DHTTYPE DHT11

const unsigned long INTERVALO_ENVIO_MS = 30000;   // cada 30 s, igual que el dashboard
const unsigned long REINTENTO_WIFI_MS = 10000;

DHT dht(DHTPIN, DHTTYPE);
Preferences memoria;
WiFiClientSecure clienteSeguro;
WiFiClient clientePlano;

String deviceId;
String deviceSecret;
String lineaSerial;
unsigned long ultimoEnvio = 0;
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
  Serial.println("Este sensor no esta vinculado.");
  Serial.println("Crea el sensor en el panel /admin, escribe aqui el codigo (ej. K7QM-4XRT) y presiona Enter.");
}

/* -------------------------------------------------------------------------- */
/*                                   WiFi                                     */
/* -------------------------------------------------------------------------- */

void conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("No se pudo conectar al WiFi; se reintentara.");
  }
  ultimoIntentoWifi = millis();
}

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
  pedirCodigo();
}

/* -------------------------------------------------------------------------- */
/*                          Vinculacion y telemetria                          */
/* -------------------------------------------------------------------------- */

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
    Serial.println("La llave quedo guardada en la memoria del ESP32. Primer envio en unos segundos.");
    ultimoEnvio = millis() - INTERVALO_ENVIO_MS + 3000;
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

// Confirma al arrancar que la llave guardada sigue siendo valida
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
  } else {
    Serial.print("No se pudo verificar la llave (");
    Serial.print(estado);
    Serial.println("); se intentara con el primer envio.");
  }
}

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

void enviarLectura() {
  float temperatura, humedad;
  if (!leerSensor(temperatura, humedad)) {
    Serial.println("Error al leer el DHT11. Revisa las conexiones.");
    return;
  }

  float temperaturaF = dht.convertCtoF(temperatura);
  float sensacion = dht.computeHeatIndex(temperatura, humedad, false);

  Serial.print("Humedad: ");
  Serial.print(humedad);
  Serial.print(" %  |  Temperatura: ");
  Serial.print(temperatura);
  Serial.print(" °C / ");
  Serial.print(temperaturaF);
  Serial.print(" °F  |  Sensacion termica: ");
  Serial.print(sensacion);
  Serial.println(" °C");

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
    Serial.print("WiFi: ");
    Serial.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "desconectado");
    Serial.print("Sensor: ");
    Serial.println(estaVinculado() ? deviceId : "sin vincular");
  } else if (comando == "olvidar") {
    olvidarCredenciales();
    Serial.println("Llave borrada.");
    pedirCodigo();
  } else if (!estaVinculado()) {
    vincular(linea);
  } else {
    Serial.println("Comandos: estado | olvidar. (Ya esta vinculado; usa \"olvidar\" para vincularlo de nuevo.)");
  }
}

void leerSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineaSerial.length()) procesarComando(lineaSerial);
      lineaSerial = "";
    } else if (lineaSerial.length() < 64) {
      lineaSerial += c;
    }
  }
}

/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("¿Se puede salir? - sensor de temperatura v" FIRMWARE_VERSION);

  dht.begin();

  if (usaHttps()) {
    // Valida el certificado del servidor: sin esto alguien podria hacerse pasar por la API
    clienteSeguro.setCACert(ROOT_CA);
  } else {
    Serial.println("AVISO: API_URL usa http://. Solo para pruebas en tu red local.");
  }

  conectarWifi();
  cargarCredenciales();

  if (!estaVinculado()) {
    pedirCodigo();
  } else if (WiFi.status() == WL_CONNECTED) {
    verificarLlave();
  }
}

void loop() {
  leerSerial();

  if (!estaVinculado() || !wifiListo()) return;

  if (millis() - ultimoEnvio >= INTERVALO_ENVIO_MS) {
    ultimoEnvio = millis();
    enviarLectura();
  }
}
