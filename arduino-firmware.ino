#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

#define DEVICE_ID "kit-001"
#define ONE_WIRE_BUS 4

const char* WIFI_SSID = "SpectrumSetup-83";
const char* WIFI_PASS = "benchkettle897";

// ---- MQTT broker ----
const bool USE_TLS = true;
const char* MQTT_HOST = "849c76960d114c7ab5f1088f0c99936a.s1.eu.hivemq.cloud";  // HiveMQ/EMQX hostname
const int MQTT_PORT = 8883;
const char* MQTT_USER = "twilio";
const char* MQTT_PASS = "twilioSF2008";

// ----------------------
WiFiClientSecure tlsClient;
WiFiClient plainClient;
Client& netClient = (USE_TLS ? (Client&)tlsClient : (Client&)plainClient);
PubSubClient mqtt(netClient);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

String topicCmd = String("devices/") + DEVICE_ID + "/cmd";
String topicResp = String("devices/") + DEVICE_ID + "/resp";
String topicHB = String("devices/") + DEVICE_ID + "/heartbeat";

bool mqttWasConnected = false;
unsigned long lastHb = 0;
const String CMD_TOPIC = String("devices/") + DEVICE_ID + "/cmd";
const String HB_TOPIC = String("devices/") + DEVICE_ID + "/heartbeat";

void publishHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["device_id"] = DEVICE_ID;
  doc["ts"] = millis() / 1000;
  char buf[128];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topicHB.c_str(), buf, n);
}

void handleCmd(char* topic, byte* payload, unsigned int len) {
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload, len)) return;

  const char* cmd = doc["cmd"] | "";
  const char* req = doc["req_id"] | "";
  if (strcmp(cmd, "read_temp") != 0) return;

  sensors.requestTemperatures();
  float c = sensors.getTempCByIndex(0);
  float f = c * 9.0f / 5.0f + 32.0f;

  StaticJsonDocument<256> out;
  out["req_id"] = req;
  out["device"] = DEVICE_ID;
  out["temp_c"] = c;
  out["temp_f"] = f;
  out["ts"] = millis() / 1000;

  char buf[256];
  size_t n = serializeJson(out, buf, sizeof(buf));
  mqtt.publish(topicResp.c_str(), buf, n);
}

void ensureMqtt() {
  while (!mqtt.connected()) {
    String cid = "esp32-" + String(DEVICE_ID);
    bool ok = (strlen(MQTT_USER) == 0)
                ? mqtt.connect(cid.c_str())
                : mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS);

    if (ok) {
      mqtt.subscribe(topicCmd.c_str());
      publishHeartbeat();
      Serial.println("MQTT connected");
    } else {
      Serial.print("MQTT connect failed, state=");
      Serial.println(mqtt.state());
      delay(1000);
    }
  }
}

void blinkStatus(int blinks, int delayMs = 150) {
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_BUILTIN, HIGH);
    delay(delayMs);
    digitalWrite(LED_BUILTIN, LOW);
    delay(delayMs);
  }
}



void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  // Boot indicator (3 quick blinks)
  blinkStatus(3, 100);

  // Serial init (keep only one Serial.begin)
  Serial.begin(115200);

  unsigned long sStart = millis();
  while (!Serial && (millis() - sStart) < 1500) {
    delay(10);
  }  // Wait for serial port to connect (Nano ESP32)
  Serial.println("Booting up...");

  // Start sensors
  sensors.begin();

  // ---- Wi-Fi connect ----
  Serial.print("Connecting WiFi to: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long tStart = millis();
  int dots = 0;
  while (WiFi.status() != WL_CONNECTED && (millis() - tStart) < 15000UL) {  // 15s max
    delay(500);
    Serial.print(".");
    dots++;
    // tiny LED nibble every second while waiting
    if (dots % 2 == 0) {
      digitalWrite(LED_BUILTIN, HIGH);
      delay(40);
      digitalWrite(LED_BUILTIN, LOW);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    blinkStatus(2, 200);  // Wi-Fi OK
  } else {
    Serial.println("\nWiFi failed!");
    blinkStatus(10, 100);  // Wi-Fi fail
    // Optional: reboot after a short pause to try again
    delay(2000);
    ESP.restart();
  }

  // ---- MQTT client config (set server before connect) ----
  if (USE_TLS) {
    // DEV ONLY: skip certificate validation. For production, load CA with setCACert(...)
    tlsClient.setInsecure();
  }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(handleCmd);

  // ---- MQTT connect ----
  Serial.println("Connecting to MQTT...");

  // Build a unique client ID
  String clientId = "esp32-" + String(DEVICE_ID);

  // Try to connect with username and password
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
    Serial.println("MQTT connected!");
    blinkStatus(5, 100);  // 5 quick = MQTT OK
  } else {
    Serial.print("MQTT failed, rc=");
    Serial.println(mqtt.state());
    blinkStatus(3, 500);  // 3 slow = MQTT fail
  }
}

void loop() {
  // Optional: hint if Wi-Fi dropped
  if (WiFi.status() != WL_CONNECTED) {
    blinkStatus(3, 80);  // 3 quick = Wi-Fi drop
    delay(300);
  }

  // Maintain MQTT connection
  if (!mqtt.connected()) {
    if (mqttWasConnected) {
      mqttWasConnected = false;
      blinkStatus(3, 500);  // 3 slow = MQTT lost
    }

    // Small backoff between attempts
    static unsigned long lastAttempt = 0;
    if (millis() - lastAttempt > 2000) {
      lastAttempt = millis();

      // Build a unique client ID and connect with USER/PASS
      String clientId = "esp32-" + String(DEVICE_ID);
      if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
        mqttWasConnected = true;
        blinkStatus(5, 100);                // 5 quick = MQTT connected
        mqtt.subscribe(CMD_TOPIC.c_str());  // (re)subscribe to /cmd after reconnect
      }
    }
  } else {
    // Pump MQTT client
    mqtt.loop();

    // Heartbeat every 60s + a tiny wink
    if (millis() - lastHb > 60000UL) {
      lastHb = millis();

      StaticJsonDocument<128> doc;
      doc["device_id"] = DEVICE_ID;
      doc["ts"] = (uint32_t)(millis() / 1000);
      char buf[128];
      size_t n = serializeJson(doc, buf);
      mqtt.publish(HB_TOPIC.c_str(), buf, n);

      digitalWrite(LED_BUILTIN, HIGH);
      delay(60);
      digitalWrite(LED_BUILTIN, LOW);
    }
  }
}