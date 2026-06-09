#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>

// Wi-Fi 
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

//  MQTT 
const char* MQTT_BROKER   = "broker.hivemq.com";
const int   MQTT_PORT     = 1883;
const char* TOPIC_LEITURA = "spacecrop/leitura";
const char* TOPIC_ALERTA  = "spacecrop/alerta";
const char* TOPIC_STATUS  = "spacecrop/status";
char        MQTT_CLIENT[32];

// NTP 
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET = -10800;  
const int   DST_OFFSET = 0;

// Pinos 
const int DHT_PIN     = 15;
const int LED_OK      = 2;   // Verde  — plantação OK
const int LED_ALERTA  = 4;   // Vermelho — alerta climático
const int BUZZER_PIN  = 5;
const int LED_DIA     = 26;  // Amarelo — período DIA
const int LED_NOITE   = 27;  // Azul    — período NOITE

// OLED 
#define OLED_WIDTH  128
#define OLED_HEIGHT 64
#define OLED_RESET  -1

//  Thresholds agrícolas 
const float TEMP_GEADA    = 5.0f;
const float TEMP_CALOR    = 35.0f;
const float UMID_SECA     = 30.0f;
const float UMID_ENCHARCA = 90.0f;

//  Horário solar 
const int HORA_AMANHECER = 6;   
const int HORA_ANOITECER = 18;  

// Cenários simulados 
struct Cenario { float temp; float umid; const char* cultura; };

Cenario cenarios[] = {
  { 22.0f, 60.0f, "Soja"  },
  { 24.5f, 65.0f, "Milho" },
  { 20.0f, 55.0f, "Trigo" },
  { 30.0f, 22.0f, "Soja"  },
  { 31.5f, 18.0f, "Milho" },
  { 36.0f, 45.0f, "Soja"  },
  { 38.2f, 50.0f, "Cana"  },
  { 40.1f, 48.0f, "Milho" },
  {  3.5f, 80.0f, "Trigo" },
  {  1.0f, 85.0f, "Soja"  },
  { 26.0f, 92.0f, "Arroz" },
  { 25.0f, 95.0f, "Milho" },
  { 27.0f, 68.0f, "Soja"  },
  { 23.5f, 62.0f, "Trigo" },
  { 21.0f, 58.0f, "Cana"  },
};

const int SIM_COUNT = sizeof(cenarios) / sizeof(cenarios[0]);
int simIndex = 0;

// Objetos 
DHTesp           dht;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);

//  Estado global 
String statusAtual  = "IDEAL";
String periodoAtual = "DIA";
int    leituraCount = 0;
bool   wifiOk       = false;
bool   mqttOk       = false;

unsigned long lastPublish   = 0;
unsigned long lastMqttRetry = 0;
unsigned long lastBlink     = 0;
unsigned long lastBuzz      = 0;
unsigned long lastDisplay   = 0;
bool blinkState = false;
bool buzzActive = false;
int  paginaOled = 0;

float  tempAtual    = 0;
float  umidAtual    = 0;
String culturaAtual = "";

//  Tempos 
const unsigned long PUBLISH_MS   = 4000;
const unsigned long WIFI_TIMEOUT = 8000;
const unsigned long MQTT_RETRY   = 30000;
const unsigned long BLINK_FAST   = 120;
const unsigned long BLINK_SLOW   = 600;
const unsigned long BUZZ_ON_MS   = 200;
const unsigned long BUZZ_OFF_MS  = 400;
const unsigned long BUZZ_LONG    = 700;
const unsigned long BUZZ_GAP     = 500;

// Funções 


String calcularPeriodo() {
  return (random(2) == 0) ? "DIA" : "NOITE";
}

// Atualiza os LEDs de dia/noite
void atualizarLedPeriodo() {
  if (periodoAtual == "DIA") {
    digitalWrite(LED_DIA,   HIGH);
    digitalWrite(LED_NOITE, LOW);
  } else {
    digitalWrite(LED_DIA,   LOW);
    digitalWrite(LED_NOITE, HIGH);
  }
}

void connectWiFi() {
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    Serial.println("[WiFi] Conectado! IP: " + WiFi.localIP().toString());
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
  } else {
    wifiOk = false;
    Serial.println("[WiFi] Indisponivel — modo offline");
    WiFi.disconnect(true);
  }
}

void tryMQTT() {
  if (!wifiOk) return;
  Serial.print("[MQTT] Conectando...");
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(15);
  if (mqtt.connect(MQTT_CLIENT)) {
    mqttOk = true;
    Serial.println(" OK!");
    mqtt.publish(TOPIC_STATUS, "{\"online\":true,\"msg\":\"SpaceCrop online\"}");
  } else {
    mqttOk = false;
    Serial.println(" Falhou rc=" + String(mqtt.state()));
  }
}

String getTimestamp() {
  struct tm t;
  char buf[25] = "sem_hora";
  if (wifiOk && getLocalTime(&t))
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
  return String(buf);
}

void avaliarStatus(float temp, float umid) {
  if      (temp <= TEMP_GEADA)    statusAtual = "GEADA";
  else if (temp >= TEMP_CALOR)    statusAtual = "CALOR";
  else if (umid <= UMID_SECA)     statusAtual = "SECA";
  else if (umid >= UMID_ENCHARCA) statusAtual = "ENCHARCA";
  else                            statusAtual = "IDEAL";
}

void publicarDados() {
  leituraCount++;
  String ts = getTimestamp();

  String motivo;
  if      (statusAtual == "GEADA")    motivo = "Risco de geada! Temp: " + String(tempAtual, 1) + "C";
  else if (statusAtual == "CALOR")    motivo = "Calor excessivo! Temp: " + String(tempAtual, 1) + "C";
  else if (statusAtual == "SECA")     motivo = "Seca! Umid: " + String(umidAtual, 1) + "%";
  else if (statusAtual == "ENCHARCA") motivo = "Encharcamento! Umid: " + String(umidAtual, 1) + "%";
  else                                motivo = "Sem alertas";

  // Payload leitura
  StaticJsonDocument<320> docL;
  docL["device"]      = MQTT_CLIENT;
  docL["cultura"]     = culturaAtual;
  docL["temperatura"] = round(tempAtual * 10) / 10.0;
  docL["umidade"]     = round(umidAtual * 10) / 10.0;
  docL["status"]      = statusAtual;
  docL["periodo"]     = periodoAtual;
  docL["leitura"]     = leituraCount;
  docL["timestamp"]   = ts;

  char payloadL[320];
  serializeJson(docL, payloadL);

  // Payload alerta
  StaticJsonDocument<320> docA;
  docA["alerta_ativo"] = (statusAtual != "IDEAL");
  docA["status"]       = statusAtual;
  docA["motivo"]       = motivo;
  docA["cultura"]      = culturaAtual;
  docA["temperatura"]  = tempAtual;
  docA["umidade"]      = umidAtual;
  docA["periodo"]      = periodoAtual;
  docA["leitura"]      = leituraCount;
  docA["timestamp"]    = ts;

  char payloadA[320];
  serializeJson(docA, payloadA);

  if (mqttOk && mqtt.connected()) {
    mqtt.publish(TOPIC_LEITURA, payloadL, true);
    mqtt.publish(TOPIC_ALERTA,  payloadA, true);

    StaticJsonDocument<128> docS;
    docS["online"]    = true;
    docS["uptime_s"]  = millis() / 1000;
    docS["ip"]        = WiFi.localIP().toString();
    docS["rssi"]      = WiFi.RSSI();
    docS["timestamp"] = ts;
    char payloadS[128];
    serializeJson(docS, payloadS);
    mqtt.publish(TOPIC_STATUS, payloadS, true);
    Serial.print("[MQTT] → ");
  } else {
    Serial.print("[OFFLINE] ");
  }

  Serial.printf("[#%04d] %s | %s | Temp: %.1fC | Umid: %.1f%% | %s\n",
    leituraCount, culturaAtual.c_str(), periodoAtual.c_str(),
    tempAtual, umidAtual, statusAtual.c_str());
}

void atualizarOled() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (paginaOled == 0) {
    
    display.setTextSize(1);
    display.setCursor(2, 0);
    display.print(periodoAtual == "DIA" ? "* DIA  | " : "~ NOITE| ");
    display.println(culturaAtual.substring(0, 7));
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    display.setTextSize(2);
    display.setCursor(0, 15);
    display.print(tempAtual, 1);
    display.println(" C");
    display.setCursor(0, 36);
    display.print(umidAtual, 1);
    display.println(" %");

    display.setTextSize(1);
    display.setCursor(0, 56);
    display.print("#");
    display.print(leituraCount);
    display.print(" ");
    display.print(statusAtual.substring(0, 10));

  } else {
    
    display.setTextSize(1);
    display.setCursor(2, 0);
    display.println("== STATUS ==");
    display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

    display.setCursor(0, 14);
    if (statusAtual == "IDEAL") {
      display.println("OK - Condicoes");
      display.setCursor(0, 26);
      display.println("dentro do ideal");
    } else {
      display.println("!! ALERTA !!");
      display.setCursor(0, 26);
      display.println(statusAtual);
    }

    display.setCursor(0, 42);
    display.print("Periodo: ");
    display.println(periodoAtual);

    display.setCursor(0, 54);
    display.print(mqttOk ? "MQTT: OK" : (wifiOk ? "MQTT: FALHOU" : "WiFi: OFFLINE"));
  }

  display.display();
}

void updateAlerts(unsigned long now) {
  if (statusAtual == "IDEAL") {
    digitalWrite(LED_OK,     HIGH);
    digitalWrite(LED_ALERTA, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    buzzActive = false;

  } else if (statusAtual == "SECA") {
    digitalWrite(LED_OK, LOW);
    if (now - lastBlink >= BLINK_SLOW) {
      blinkState = !blinkState;
      digitalWrite(LED_ALERTA, blinkState ? HIGH : LOW);
      lastBlink = now;
    }
    if (!buzzActive && now - lastBuzz >= 2000) {
      digitalWrite(BUZZER_PIN, HIGH); lastBuzz = now; buzzActive = true;
    } else if (buzzActive && now - lastBuzz >= 100) {
      digitalWrite(BUZZER_PIN, LOW);  lastBuzz = now; buzzActive = false;
    }

  } else if (statusAtual == "CALOR") {
    digitalWrite(LED_OK,     LOW);
    digitalWrite(LED_ALERTA, HIGH);
    if (!buzzActive && now - lastBuzz >= BUZZ_OFF_MS) {
      digitalWrite(BUZZER_PIN, HIGH); lastBuzz = now; buzzActive = true;
    } else if (buzzActive && now - lastBuzz >= BUZZ_ON_MS) {
      digitalWrite(BUZZER_PIN, LOW);  lastBuzz = now; buzzActive = false;
    }

  } else if (statusAtual == "GEADA") {
    if (now - lastBlink >= BLINK_FAST) {
      blinkState = !blinkState;
      digitalWrite(LED_ALERTA, blinkState ? HIGH : LOW);
      digitalWrite(LED_OK,     LOW);
      lastBlink = now;
    }
    if (!buzzActive && now - lastBuzz >= BUZZ_GAP) {
      digitalWrite(BUZZER_PIN, HIGH); lastBuzz = now; buzzActive = true;
    } else if (buzzActive && now - lastBuzz >= BUZZ_LONG) {
      digitalWrite(BUZZER_PIN, LOW);  lastBuzz = now; buzzActive = false;
    }

  } else {
    // ENCHARCA
    digitalWrite(LED_OK, LOW);
    if (now - lastBlink >= BLINK_FAST) {
      blinkState = !blinkState;
      digitalWrite(LED_ALERTA, blinkState ? HIGH : LOW);
      lastBlink = now;
    }
    if (!buzzActive && now - lastBuzz >= BUZZ_OFF_MS) {
      digitalWrite(BUZZER_PIN, HIGH); lastBuzz = now; buzzActive = true;
    } else if (buzzActive && now - lastBuzz >= BUZZ_ON_MS) {
      digitalWrite(BUZZER_PIN, LOW);  lastBuzz = now; buzzActive = false;
    }
  }
}

//  Setup 

void setup() {
  Serial.begin(115200);
  Serial.println("\n╔══════════════════════════════════╗");
  Serial.println("║   SpaceCrop IoT — Iniciando...   ║");
  Serial.println("╚══════════════════════════════════╝");

  snprintf(MQTT_CLIENT, sizeof(MQTT_CLIENT), "spacecrop-%08lX", (unsigned long)millis());

  pinMode(LED_OK,     OUTPUT);
  pinMode(LED_ALERTA, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_DIA,    OUTPUT);
  pinMode(LED_NOITE,  OUTPUT);
  pinMode(DHT_PIN,    INPUT_PULLUP);

  digitalWrite(LED_OK,     LOW);
  digitalWrite(LED_ALERTA, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_DIA,    LOW);
  digitalWrite(LED_NOITE,  LOW);

  dht.setup(DHT_PIN, DHTesp::DHT22);

  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] Falha ao iniciar!");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 20);
    display.println("SpaceCrop IoT");
    display.setCursor(10, 32);
    display.println("Iniciando...");
    display.display();
  }

  connectWiFi();
  if (wifiOk) tryMQTT();
  else        Serial.println("[MQTT] Pulado — sem Wi-Fi");

  periodoAtual = calcularPeriodo();
  atualizarLedPeriodo();
  Serial.println("[PERIODO] " + periodoAtual);
  Serial.println("[MODO] " + String(SIM_COUNT) + " cenarios | " + String(PUBLISH_MS/1000) + "s/leitura");
}

// Loop 

void loop() {
  unsigned long now = millis();

  
  if (mqttOk && mqtt.connected()) {
    mqtt.loop();
  } else if (wifiOk && now - lastMqttRetry >= MQTT_RETRY) {
    lastMqttRetry = now;
    tryMQTT();
  }

  
  if (now - lastPublish >= PUBLISH_MS) {
    lastPublish = now;

    // Atualiza período e LEDs de dia/noite
    String novoPeriodo = calcularPeriodo();
    if (novoPeriodo != periodoAtual) {
      periodoAtual = novoPeriodo;
      atualizarLedPeriodo();
    }

    
    tempAtual    = cenarios[simIndex].temp;
    umidAtual    = cenarios[simIndex].umid;
    culturaAtual = cenarios[simIndex].cultura;
    simIndex     = (simIndex + 1) % SIM_COUNT;

    avaliarStatus(tempAtual, umidAtual);
    publicarDados();
  }

  updateAlerts(now);

  
  if (now - lastDisplay >= 3500) {
    lastDisplay = now;
    paginaOled  = (paginaOled + 1) % 2;
    atualizarOled();
  }
}
