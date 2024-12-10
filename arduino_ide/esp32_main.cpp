#include <pgmspace.h>
#include "secrets.h"
#include <WiFiClientSecure.h>
#include <MQTTClient.h>
#include <ArduinoJson.h>
#include "WiFi.h"
#include "DHT.h"
#include <NTPClient.h>
#include <WiFiUdp.h>

// DHT11 Settings
#define DHTPIN 4       // GPIO där sensorn är ansluten
#define DHTTYPE DHT11  // Typ av sensor: DHT11
DHT dht(DHTPIN, DHTTYPE);

// NTP Settings
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000); // CET tidszon

#define AWS_IOT_PUBLISH_TOPIC "/telemetry"
#define AWS_IOT_SUBSCRIBE_TOPIC "/downlink"

long sendInterval = 10000;  // Intervallet i millisekunder för att skicka data

String THINGNAME = "DCDA0C5A94CC";
WiFiClientSecure net = WiFiClientSecure();
MQTTClient client = MQTTClient(1024);

void connectAWS() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Använd MAC-adressen som enhetens namn
  THINGNAME = WiFi.macAddress();
  for (int i = 0; i < THINGNAME.length(); i++) {
    if (THINGNAME.charAt(i) == ':') {
      THINGNAME.remove(i, 1);
      i--;
    }
  }

  Serial.println();
  Serial.print("MAC Address: ");
  Serial.println(THINGNAME);

  // Försök att ansluta till WiFi med timeout
  Serial.println("Connecting to Wi-Fi");
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
    if (wifi_attempts > 40) {  // 20 sekunders timeout
      Serial.println("Wi-Fi Connection Failed, restarting...");
      ESP.restart();
    }
  }

  // Konfigurera WiFiClientSecure för att använda AWS-certifikaten
  net.setCACert(AWS_CERT_CA);
  net.setCertificate(AWS_CERT_CRT);
  net.setPrivateKey(AWS_CERT_PRIVATE);

  // Anslut till MQTT-broker på AWS IoT endpoint
  client.begin(AWS_IOT_ENDPOINT, 8883, net);
  client.onMessage(messageHandler);

  Serial.print("Connecting to AWS IoT");
  int aws_attempts = 0;
  while (!client.connect(THINGNAME.c_str())) {
    Serial.print(".");
    delay(100);
    aws_attempts++;
    if (aws_attempts > 50) {  // 5 sekunders timeout
      Serial.println("AWS IoT Connection Failed, restarting...");
      ESP.restart();
    }
  }

  client.subscribe(THINGNAME + AWS_IOT_SUBSCRIBE_TOPIC);
  Serial.println("AWS IoT Connected!");
}

void setupShadow() {
  client.subscribe("$aws/things/" + THINGNAME + "/shadow/get/accepted");
  client.subscribe("$aws/things/" + THINGNAME + "/shadow/get/rejected");
  client.subscribe("$aws/things/" + THINGNAME + "/shadow/update/delta");
  client.publish("$aws/things/" + THINGNAME + "/shadow/get");
}

bool publishTelemetry(String payload) {
  Serial.print("Publishing: ");
  Serial.println(payload);
  return client.publish(THINGNAME + AWS_IOT_PUBLISH_TOPIC, payload);
}

void messageHandler(String &topic, String &payload) {
  Serial.println("incoming: " + topic + " - " + payload);

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  if (topic.endsWith("/shadow/get/accepted")) {
    if (doc.containsKey("state") && doc["state"].containsKey("desired")) {
      StaticJsonDocument<256> desiredState;
      desiredState.set(doc["state"]["desired"]);
      updateSettings(desiredState);
    }
  } else if (topic.endsWith("/shadow/update/delta")) {
    if (doc.containsKey("state")) {
      StaticJsonDocument<256> deltaState;
      deltaState.set(doc["state"]);
      updateSettings(deltaState);
    }
  }
}

void updateSettings(JsonDocument &settingsObj) {
  sendInterval = settingsObj["sendIntervalSeconds"].as<int>() * 1000;

  StaticJsonDocument<512> docResponse;
  docResponse["state"]["reported"] = settingsObj;
  char jsonBuffer[512];
  serializeJson(docResponse, jsonBuffer);

  Serial.print("Sending reported state to AWS: ");
  serializeJson(docResponse, Serial);

  client.publish("$aws/things/" + THINGNAME + "/shadow/update", jsonBuffer);
}

void setup() {
  Serial.begin(115200);
  dht.begin();  // Starta DHT-sensorn
  delay(2000);
  connectAWS();
  setupShadow();

  // Starta NTP klienten
  timeClient.begin();
  timeClient.update();
}

void loop() {
  static unsigned long previousMillis = -sendInterval;

  if (!client.connected()) {
    connectAWS();  // Försök att återansluta om anslutningen till AWS tappas
  }

  client.loop();
  timeClient.update();  // Uppdatera NTP-tiden

  if (millis() - previousMillis >= sendInterval) {
    previousMillis = millis();

    // Läser data från DHT-sensorn
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    // Kontrollera om avläsningarna är giltiga
    if (isnan(temperature) || isnan(humidity)) {
      Serial.println("Failed to read from DHT sensor!");
      return;
    }
    
    // Skriv ut värden till Serial Monitor
    Serial.print("Temperature: ");
    Serial.println(temperature);
    Serial.print("Humidity: ");
    Serial.println(humidity);

    // Skapa JSON-payload och inkludera riktig tidsstämpel från NTP
    String timestamp = timeClient.getFormattedTime();
    
    // Här konverterar vi deviceId till device_id innan vi skickar till MQTT och eventuellt till DynamoDB
    String payload = "{\"device_id\":\"" + THINGNAME + "\",\"temperature\":" + String(temperature) + ",\"humidity\":" + String(humidity) + ",\"timestamp\":\"" + timestamp + "\"}";

    bool sendResult = publishTelemetry(payload);
    if (!sendResult) {
      ESP.restart();
    }
  }
}
