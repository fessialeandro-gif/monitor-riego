/*
  Monitor de humedad de suelo — 1 sensor
  ESP8266 (NodeMCU / Wemos D1 mini) + sensor capacitivo en A0 (+ DHT22 opcional en D1)
  Envía una lectura cada INTERVALO_MS a una Netlify Function (/api/lecturas).

  Librerías (solo si USAR_DHT = true): "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor"
  Placa: NodeMCU 1.0 (ESP-12E Module) o LOLIN(WEMOS) D1 mini
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>

// ================== CONFIGURACIÓN ==================
const char* WIFI_SSID    = "EPEC WiFi";
const char* WIFI_PASS    = "Chupalaputo";
const char* API_URL      = "https://TU-SITIO.netlify.app/api/lecturas";
const char* DEVICE_TOKEN = "EL_MISMO_TOKEN_QUE_PUSISTE_EN_NETLIFY";

const unsigned long INTERVALO_MS = 5UL * 60UL * 1000UL;  // cada 5 minutos

// true = no envía nada, solo imprime el valor crudo por Serial para calibrar
const bool MODO_CALIBRACION = false;

// Calibración (valor crudo 0-1023)
// SECO = sensor al aire. MOJADO = sensor en un vaso de agua (hasta la línea).
const int CAL_SECO   = 780;
const int CAL_MOJADO = 370;

// DHT22 opcional: poné false si no lo tenés conectado
#define USAR_DHT true
const uint8_t PIN_SENSOR = A0;
const uint8_t PIN_DHT    = D1;
#define DHT_TYPE DHT22        // DHT11 si usás ese
// ===================================================

#if USAR_DHT
  #include <DHT.h>
  DHT dht(PIN_DHT, DHT_TYPE);
#endif

unsigned long ultimoEnvio = 0;
bool primerEnvio = true;

int leerCrudo() {
  analogRead(PIN_SENSOR);  // primera lectura se descarta
  long suma = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) {
    suma += analogRead(PIN_SENSOR);
    delay(5);
  }
  return suma / N;
}

// 0-100, o -1 si el valor no tiene sentido (sensor desconectado / en corto)
int crudoAPorcentaje(int crudo) {
  if (crudo < 10 || crudo > 1015) return -1;
  if (CAL_SECO == CAL_MOJADO) return -1;
  long p = (long)(CAL_SECO - crudo) * 100L / (CAL_SECO - CAL_MOJADO);
  return constrain(p, 0, 100);
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" OK ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" FALLÓ (reintenta en el próximo ciclo)");
  }
}

String armarJson() {
  int crudo = leerCrudo();
  int pct = crudoAPorcentaje(crudo);

  String json = "{\"s1\":";
  if (pct < 0) json += "null"; else json += pct;

#if USAR_DHT
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  json += ",\"temperatura\":";
  if (isnan(t)) json += "null"; else json += String(t, 1);
  json += ",\"humedad_aire\":";
  if (isnan(h)) json += "null"; else json += String(h, 1);
#endif

  json += "}";
  Serial.printf("Crudo=%d -> ", crudo);
  return json;
}

bool enviar(const String& json) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();  // sin validar certificado (simple y suficiente para esto)

  HTTPClient https;
  if (!https.begin(*client, API_URL)) {
    Serial.println("No se pudo iniciar HTTPS");
    return false;
  }
  https.setTimeout(15000);
  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Token", DEVICE_TOKEN);

  int code = https.POST(json);
  Serial.printf("POST -> %d\n", code);
  if (code < 200 || code >= 300) Serial.println(https.getString());
  https.end();
  return code >= 200 && code < 300;
}

void setup() {
  Serial.begin(115200);
  delay(200);
#if USAR_DHT
  dht.begin();
#endif
  Serial.println("\nMonitor de riego (1 sensor) iniciado");
  if (!MODO_CALIBRACION) conectarWiFi();
}

void loop() {
  if (MODO_CALIBRACION) {
    Serial.printf("Crudo = %d\n", leerCrudo());
    delay(1000);
    return;
  }

  if (primerEnvio || millis() - ultimoEnvio >= INTERVALO_MS) {
    primerEnvio = false;
    ultimoEnvio = millis();

    conectarWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      String json = armarJson();
      Serial.println(json);
      if (!enviar(json)) {
        delay(3000);
        enviar(json);  // un reintento
      }
    }
  }
  delay(50);
}
