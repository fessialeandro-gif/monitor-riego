/*
  Monitor y riego — 4 sensores de suelo + DHT22 + 2 relés
  ESP8266 (NodeMCU 1.0)

  Sensores: cada uno se alimenta desde un pin (se prenden de a uno) y las 4 salidas
            van a A0 a través de un diodo cada una (banda hacia A0) + 100 kΩ de A0 a GND.
  Zonas:    Zona A = sensores 1 y 2 → relé A      Zona B = sensores 3 y 4 → relé B

  Riego:    - Se prende SOLO cuando lo pedís desde la web y el promedio de la zona está seco.
            - Se corta solo cuando: el promedio llega a "cortar al llegar a", pasa el tiempo
              máximo, un sensor de la zona se queda sin señal, o lo detenés desde la web.
            - Todos los cortes funcionan aunque se caiga internet (los decide el ESP).
            - Al prender/reiniciar el ESP, los relés arrancan APAGADOS y no retoma riegos viejos.

  Librerías (Gestor de librerías): ArduinoJson (Benoit Blanchon, v7)
                                   DHT sensor library + Adafruit Unified Sensor (si USAR_DHT)
  Placa: NodeMCU 1.0 (ESP-12E Module)
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ArduinoJson.h>

// ================== CONFIGURACIÓN ==================
const char* WIFI_SSID    = "TU_WIFI";
const char* WIFI_PASS    = "TU_PASSWORD";
const char* BASE_URL     = "https://TU-SITIO.netlify.app";   // sin barra al final
const char* DEVICE_TOKEN = "TU_TOKEN";

// true = no se conecta a nada: imprime el valor crudo de cada sensor para calibrar
const bool MODO_CALIBRACION = false;

// Calibración por sensor (valor crudo). SECO = al aire. MOJADO = en un vaso de agua.
// Con los diodos cada sensor da distinto: calibralos uno por uno.
int CAL_SECO[4]   = {600, 600, 600, 600};
int CAL_MOJADO[4] = {250, 250, 250, 250};

// Pines (número de GPIO; entre paréntesis lo que dice la placa)
const uint8_t PIN_ALIM[4] = {14, 12, 13, 4};  // alimentación sensores 1..4: (D5) (D6) (D7) (D2)
const uint8_t PIN_RELE[2] = {5, 16};          // relé Zona A (D1), relé Zona B (D0)
const bool RELE_ACTIVO_BAJO = true;           // la mayoría de los módulos de relé se activan con LOW

#define USAR_DHT true
const uint8_t PIN_DHT = 2;                    // (D4)
#define DHT_TYPE DHT22

// Tiempos
const unsigned long ESPERA_SENSOR_MS  = 300;     // cuánto espera tras prender un sensor antes de leerlo
const unsigned long LECTURA_MS        = 10000;   // lectura normal: cada 10 s
const unsigned long LECTURA_RIEGO_MS  = 3000;    // regando: cada 3 s (para cortar a tiempo)
const unsigned long CONSULTA_MS       = 15000;   // busca órdenes de la web cada 15 s
const unsigned long CONSULTA_RIEGO_MS = 5000;    // regando: cada 5 s (para que "Detener" responda rápido)
const int           CAMBIO_MIN        = 3;       // manda lectura si algún sensor cambió 3% o más
const unsigned long ENVIO_MIN_MS      = 20000;   // ...pero no más seguido que cada 20 s
const unsigned long ENVIO_RIEGO_MS    = 10000;   // regando: hasta cada 10 s
const unsigned long INTERVALO_MS      = 300000;  // y si no cambia nada, cada 5 min igual
// ===================================================

#if USAR_DHT
  #include <DHT.h>
  DHT dht(PIN_DHT, DHT_TYPE);
#endif

struct Zona {
  const char* id;
  uint8_t s1, s2;          // índices de sensores (0..3)
  int umbral, objetivo, maxMin;   // valores por defecto; se pisan con los de la web
  bool on;
  unsigned long desde;
  long ultimoId;
};
Zona zonas[2] = {
  {"A", 0, 1, 30, 45, 10, false, 0, -1},
  {"B", 2, 3, 30, 45, 10, false, 0, -1},
};

struct Evento { char zona; const char* tipo; int valor; };
Evento eventos[8];
int nEventos = 0;

int pct[4] = {-1, -1, -1, -1};
int pctEnviado[4] = {-100, -100, -100, -100};
float temperatura = NAN, humedadAire = NAN;

bool idsListos = false;       // la primera respuesta del servidor solo sincroniza, no ejecuta órdenes viejas
bool forzarEnvio = false;
bool primeraVez = true;
unsigned long ultLectura = 0, ultEnvio = 0, ultConsulta = 0, proxReintento = 0;

// ---------------- Relés ----------------
void setRele(int z, bool on) {
  bool nivel = RELE_ACTIVO_BAJO ? !on : on;
  digitalWrite(PIN_RELE[z], nivel ? HIGH : LOW);
}

void agregarEvento(int z, const char* tipo, int valor) {
  if (nEventos >= 8) { for (int i = 1; i < 8; i++) eventos[i - 1] = eventos[i]; nEventos = 7; }
  eventos[nEventos++] = { zonas[z].id[0], tipo, valor };
  forzarEnvio = true;
}

int promedioZona(int z) {
  int a = pct[zonas[z].s1], b = pct[zonas[z].s2];
  if (a < 0 || b < 0) return -1;
  return (a + b + 1) / 2;
}

void arrancarRiego(int z) {
  int p = promedioZona(z);
  zonas[z].on = true;
  zonas[z].desde = millis();
  setRele(z, true);
  Serial.printf(">>> Zona %s: RIEGO ON (promedio %d%%)\n", zonas[z].id, p);
  agregarEvento(z, "inicio", p);
}

void cortarRiego(int z, const char* motivo) {
  int p = promedioZona(z);
  zonas[z].on = false;
  setRele(z, false);
  Serial.printf(">>> Zona %s: RIEGO OFF (%s, promedio %d%%)\n", zonas[z].id, motivo, p);
  agregarEvento(z, motivo, p);
}

// Protecciones locales: se chequean en cada lectura, haya internet o no
void chequeoSeguridad() {
  for (int z = 0; z < 2; z++) {
    if (!zonas[z].on) continue;
    int p = promedioZona(z);
    if (p < 0)                                                    cortarRiego(z, "corte_sin_senal");
    else if (p >= zonas[z].objetivo)                              cortarRiego(z, "corte_objetivo");
    else if (millis() - zonas[z].desde >= (unsigned long)zonas[z].maxMin * 60000UL) cortarRiego(z, "corte_tiempo");
  }
}

// ---------------- Sensores ----------------
int leerCrudo(int i) {
  digitalWrite(PIN_ALIM[i], HIGH);
  delay(ESPERA_SENSOR_MS);
  analogRead(A0);
  long suma = 0;
  for (int k = 0; k < 16; k++) { suma += analogRead(A0); delay(3); }
  digitalWrite(PIN_ALIM[i], LOW);
  delay(30);   // que A0 baje antes del próximo sensor (por eso la 100 kΩ a GND)
  return suma / 16;
}

int crudoAPorcentaje(int crudo, int i) {
  if (crudo < 15) return -1;                       // sin señal: sensor desconectado
  if (CAL_SECO[i] == CAL_MOJADO[i]) return -1;
  long p = (long)(CAL_SECO[i] - crudo) * 100L / (CAL_SECO[i] - CAL_MOJADO[i]);
  return constrain(p, 0, 100);
}

void leerTodo() {
  for (int i = 0; i < 4; i++) {
    int c = leerCrudo(i);
    pct[i] = crudoAPorcentaje(c, i);
    Serial.printf("S%d=%d(%d%%) ", i + 1, c, pct[i]);
  }
#if USAR_DHT
  temperatura = dht.readTemperature();
  humedadAire = dht.readHumidity();
  Serial.printf("T=%.1f H=%.1f ", temperatura, humedadAire);
#endif
  Serial.printf("| A=%d%%%s B=%d%%%s\n", promedioZona(0), zonas[0].on ? "(ON)" : "", promedioZona(1), zonas[1].on ? "(ON)" : "");
}

// ---------------- Red ----------------
// La conexión se hace una vez en setup() y después el ESP se reconecta solo.
// Nunca se queda esperando acá: si no hay WiFi, sigue leyendo sensores y cortando riegos igual.
void iniciarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) { Serial.print(" OK "); Serial.println(WiFi.localIP()); }
  else Serial.println(" todavía no (sigue intentando solo)");
}

// Hace la petición y deja la respuesta JSON en "resp". Devuelve true si salió bien (2xx).
bool pedir(const char* metodo, const String& ruta, const String& cuerpo, JsonDocument& resp) {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("Sin WiFi, se reintenta después"); return false; }

  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  HTTPClient https;
  if (!https.begin(*client, String(BASE_URL) + ruta)) return false;
  https.setTimeout(12000);
  https.addHeader("X-Token", DEVICE_TOKEN);

  int code;
  if (strcmp(metodo, "POST") == 0) {
    https.addHeader("Content-Type", "application/json");
    code = https.POST(cuerpo);
  } else {
    code = https.GET();
  }
  bool ok = code >= 200 && code < 300;
  if (ok) {
    DeserializationError err = deserializeJson(resp, https.getString());
    if (err) { Serial.printf("JSON inválido: %s\n", err.c_str()); ok = false; }
  } else {
    Serial.printf("%s %s -> %d %s\n", metodo, ruta.c_str(), code, https.getString().c_str());
  }
  https.end();
  return ok;
}

// Aplica configuración y órdenes que vienen del servidor
void procesarEstado(JsonDocument& doc) {
  for (int z = 0; z < 2; z++) {
    JsonObject cz = doc["config"]["zonas"][zonas[z].id];
    if (!cz.isNull()) {
      zonas[z].umbral   = cz["umbral"]   | zonas[z].umbral;
      zonas[z].objetivo = cz["objetivo"] | zonas[z].objetivo;
      zonas[z].maxMin   = cz["maxMin"]   | zonas[z].maxMin;
    }
  }
  for (int z = 0; z < 2; z++) {
    JsonObject oz = doc["control"][zonas[z].id];
    if (oz.isNull()) continue;
    long id = oz["id"] | 0L;
    const char* orden = oz["orden"] | "detener";

    if (!idsListos) { zonas[z].ultimoId = id; continue; }   // al arrancar no ejecuta órdenes viejas
    if (id == zonas[z].ultimoId) continue;
    zonas[z].ultimoId = id;

    if (strcmp(orden, "regar") == 0 && !zonas[z].on) {
      int p = promedioZona(z);
      if (p >= 0 && p < zonas[z].umbral) arrancarRiego(z);
      else {
        Serial.printf(">>> Zona %s: pedido de riego RECHAZADO (promedio %d%%, umbral %d%%)\n", zonas[z].id, p, zonas[z].umbral);
        agregarEvento(z, "rechazado", p);
      }
    } else if (strcmp(orden, "detener") == 0 && zonas[z].on) {
      cortarRiego(z, "corte_manual");
    }
  }
  idsListos = true;
  chequeoSeguridad();   // por si cambió el objetivo o el tiempo máximo
}

bool enviarLecturas() {
  JsonDocument doc;
  for (int i = 0; i < 4; i++) {
    String k = "s" + String(i + 1);
    if (pct[i] < 0) doc[k] = nullptr; else doc[k] = pct[i];
  }
  if (isnan(temperatura)) doc["temperatura"] = nullptr; else doc["temperatura"] = temperatura;
  if (isnan(humedadAire)) doc["humedad_aire"] = nullptr; else doc["humedad_aire"] = humedadAire;
  doc["rA"] = zonas[0].on ? 1 : 0;
  doc["rB"] = zonas[1].on ? 1 : 0;
  JsonArray ev = doc["eventos"].to<JsonArray>();
  for (int i = 0; i < nEventos; i++) {
    JsonObject e = ev.add<JsonObject>();
    char zs[2] = { eventos[i].zona, 0 };
    e["zona"] = zs;
    e["tipo"] = eventos[i].tipo;
    if (eventos[i].valor < 0) e["valor"] = nullptr; else e["valor"] = eventos[i].valor;
  }
  String cuerpo;
  serializeJson(doc, cuerpo);
  Serial.println("POST " + cuerpo);

  JsonDocument resp;
  int enviados = nEventos;
  if (!pedir("POST", "/api/lecturas", cuerpo, resp)) return false;

  // sacar de la cola solo los eventos que se mandaron
  for (int i = enviados; i < nEventos; i++) eventos[i - enviados] = eventos[i];
  nEventos -= enviados;
  for (int i = 0; i < 4; i++) pctEnviado[i] = pct[i];
  ultEnvio = millis();
  ultConsulta = millis();     // la respuesta ya trae las órdenes
  forzarEnvio = nEventos > 0;
  procesarEstado(resp);
  return true;
}

void consultarOrdenes() {
  JsonDocument resp;
  ultConsulta = millis();
  if (pedir("GET", "/api/estado?esp=1", "", resp)) procesarEstado(resp);
}

// ---------------- Setup / Loop ----------------
void setup() {
  // relés apagados ANTES de configurar el pin como salida (evita un "clic" al arrancar)
  for (int z = 0; z < 2; z++) { setRele(z, false); pinMode(PIN_RELE[z], OUTPUT); setRele(z, false); }
  for (int i = 0; i < 4; i++) { pinMode(PIN_ALIM[i], OUTPUT); digitalWrite(PIN_ALIM[i], LOW); }

  Serial.begin(115200);
  delay(200);
#if USAR_DHT
  dht.begin();
#endif
  Serial.println("\nMonitor de riego (4 sensores, 2 zonas) iniciado");
  if (!MODO_CALIBRACION) iniciarWiFi();
}

void loop() {
  if (MODO_CALIBRACION) {
    for (int i = 0; i < 4; i++) Serial.printf("S%d=%4d   ", i + 1, leerCrudo(i));
    Serial.println();
    delay(700);
    return;
  }

  unsigned long ahora = millis();
  bool regando = zonas[0].on || zonas[1].on;

  // 1) Leer sensores y aplicar protecciones
  if (primeraVez || ahora - ultLectura >= (regando ? LECTURA_RIEGO_MS : LECTURA_MS)) {
    ultLectura = ahora;
    leerTodo();
    chequeoSeguridad();
    regando = zonas[0].on || zonas[1].on;

    int cambio = 0;
    for (int i = 0; i < 4; i++) {
      if ((pct[i] < 0) != (pctEnviado[i] < 0)) cambio = 100;
      else cambio = max(cambio, abs(pct[i] - pctEnviado[i]));
    }
    unsigned long desde = millis() - ultEnvio;
    if (primeraVez || desde >= INTERVALO_MS ||
        (cambio >= CAMBIO_MIN && desde >= ENVIO_MIN_MS) ||
        (regando && cambio >= 1 && desde >= ENVIO_RIEGO_MS)) {
      forzarEnvio = true;
    }
    primeraVez = false;
  }

  // 2) Mandar lecturas (y eventos de riego) cuando corresponde
  if (forzarEnvio && (long)(millis() - proxReintento) >= 0) {
    if (!enviarLecturas()) proxReintento = millis() + 10000;   // si falló, reintenta en 10 s
  }

  // 3) Buscar órdenes de la web
  if (millis() - ultConsulta >= (regando ? CONSULTA_RIEGO_MS : CONSULTA_MS)) consultarOrdenes();

  delay(20);
}
