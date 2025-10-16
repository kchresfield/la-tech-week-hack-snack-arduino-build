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
const char* MQTT_HOST = "849c76960d114c7ab5f1088f0c99936a.s1.eu.hivemq.cloud"; // HiveMQ/EMQX hostname
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "twilio";
const char* MQTT_PASS = "twilioSF2008";

// ----------------------
WiFiClientSecure tlsClient;
WiFiClient       plainClient;
Client&          netClient = (USE_TLS ? (Client&)tlsClient : (Client&)plainClient);
PubSubClient     mqtt(netClient);

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

String topicCmd  = String("devices/") + DEVICE_ID + "/cmd";
String topicResp = String("devices/") + DEVICE_ID + "/resp";
String topicHB   = String("devices/") + DEVICE_ID + "/heartbeat";

void publishHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["device_id"] = DEVICE_ID;
  doc["ts"] = millis()/1000;
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
  float f = c * 9.0f/5.0f + 32.0f;

  StaticJsonDocument<256> out;
  out["req_id"] = req;
  out["device"] = DEVICE_ID;
  out["temp_c"] = c;
  out["temp_f"] = f;
  out["ts"]     = millis()/1000;

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

void setup() {
  Serial.begin(115200);          // Start serial connection
  while (!Serial) { ; }          // Wait for serial port to connect (important on Nano ESP32)
  Serial.println("Booting up...");
  sensors.begin();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println(" connected");

  if (USE_TLS) {
    // DEV ONLY: skip certificate validation. For production, load CA:
    // tlsClient.setCACert(root_ca_pem);
    tlsClient.setInsecure();
  }

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(handleCmd);
}

void loop() {
  if (!mqtt.connected()) ensureMqtt();
  mqtt.loop();

  static unsigned long lastHB = 0;
  if (millis() - lastHB > 60000UL) { // heartbeat every 60s
    publishHeartbeat();
    lastHB = millis();
  }
}