#define FIRSTSENSOR 3
#define SECONDSENSOR 5
#define SAMPLESIZERPS 10
#define BURST_TIMEOUT_MS 500 // ms to wait after the last shot to finalize RPS calculation

float DISTANCEACROSS = 70.0;
float BBWEIGHT = 0.28;

#include <WiFi.h>
#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include "LittleFS.h"

#include <ElegantOTA.h>
#include <WebSerial.h>
AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
const byte DNS_PORT = 53;

void initPins() {
  pinMode(FIRSTSENSOR, INPUT);
  pinMode(SECONDSENSOR, INPUT);
  Serial.println("Sensors Initialized");
}

void initLittleFS() {
  if (!LittleFS.begin()) {
    Serial.println("An error has occurred while mounting LittleFS");
  }
  Serial.println("LittleFS mounted successfully");
}

void initWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Chronograph");
  Serial.println("WiFi Initialized");
}

void initNetwork() {
  // Initialize DNS server for captive portal
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  
  // Route handler for root path
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  
  // Route handler for style.css
  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });
  
  // Route handler for script.js
  webServer.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/script.js", "application/javascript");
  });
  
  // Captive portal detection - redirect all requests to the root
  webServer.onNotFound([](AsyncWebServerRequest *request){
    request->redirect("/");
  });

  ElegantOTA.begin(&webServer);

  ws.onEvent(onEvent);
  webServer.addHandler(&ws);

  webServer.begin();
  Serial.println("Captive Portal Initialized");
}

void initInterrupts() {
  attachInterrupt(digitalPinToInterrupt(FIRSTSENSOR), getFirstSensor, FALLING);
  attachInterrupt(digitalPinToInterrupt(SECONDSENSOR), getSecondSensor, FALLING);
  Serial.println("Interrupts Initialized");
}

void handleWebSocketMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    JSONVar receivedData = JSON.parse((char*)data);

    if (JSON.typeof(receivedData) == "undefined") {
      Serial.println("Parsing input failed!");
      return;
    }

    if (receivedData.hasOwnProperty("bbWeight")) {
      BBWEIGHT = (double)receivedData["bbWeight"];
      Serial.print("Updated BB Weight to: ");
      Serial.println(BBWEIGHT);
    }

    if (receivedData.hasOwnProperty("distanceAcross")) {
      DISTANCEACROSS = (double)receivedData["distanceAcross"];
      Serial.print("Updated Sensor Distance to: ");
      Serial.println(DISTANCEACROSS);
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      JSONVar settings;
      settings["bbWeight"] = roundValue(BBWEIGHT,2);
      settings["distanceAcross"] = roundValue(DISTANCEACROSS,1);
      client->text(JSON.stringify(settings));
      break;
    }
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(client, arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  initPins();
  initWiFi();
  initLittleFS();
  initNetwork();
  initInterrupts();
}


float speed_in_meters = 0, joules = 0, rounds_per_second = 0 , last_rounds_per_second = 0;

// Volatile variables modified by ISRs
volatile unsigned long ts_first = 0;
volatile unsigned long ts_second = 0;
volatile int rps_shot_count = 0;
volatile unsigned long rps_first_shot_ts = 0;
volatile unsigned long rps_last_shot_ts = 0;

// Pending variables for the main loop
unsigned long ts_first_pending = 0;
unsigned long ts_second_pending = 0;



void IRAM_ATTR getFirstSensor() {
  unsigned long now = millis();
  ts_first = now;
  if (rps_shot_count == 0) { // First shot of a new burst
    rps_first_shot_ts = now;
  }
  rps_last_shot_ts = now;
  rps_shot_count++;
}

void IRAM_ATTR getSecondSensor() {
  ts_second = millis();
}

double roundValue(double value,int precision) {
  return (int)(value * pow10(precision) + 0.5) / pow10(precision);
}

String packData(float speed, float energy, float rps) {
  JSONVar data;
  data["metric"] = roundValue(speed,1);
  data["joules"] = roundValue(energy,1);
  data["rps"] = roundValue(rps,1);
  return JSON.stringify(data);
}

void copySensorTimestamps() {
  // Safely copy the volatile timestamps to pending variables
  // This critical section is extremely short and won't cause missed interrupts
  noInterrupts();
  if (ts_first) {
    ts_first_pending = ts_first;
    ts_first = 0;
  }
  if (ts_second) {
    ts_second_pending = ts_second;
    ts_second = 0;
  }
  interrupts();
}

void processShot() {
  if (ts_first_pending && ts_second_pending) {
    if (ts_second_pending > ts_first_pending) {
      float time_diff_s = (ts_second_pending - ts_first_pending) / 1000.0;
      speed_in_meters = (DISTANCEACROSS / 1000.0) / time_diff_s;
      joules = (BBWEIGHT / 2000.0) * speed_in_meters * speed_in_meters;

      ws.textAll(packData(speed_in_meters, joules, last_rounds_per_second));
    }
    ts_first_pending = 0;
    ts_second_pending = 0;
  }
}

void handleRps() {
  int shot_count = rps_shot_count;
  unsigned long last_shot_ts = rps_last_shot_ts;

  // Check if a burst of fire has ended.
  if (shot_count > 0 && (millis() - last_shot_ts > BURST_TIMEOUT_MS)) {
    if (shot_count > 1) {
      unsigned long first_shot_ts = rps_first_shot_ts;
      unsigned long burst_duration_ms = last_shot_ts - first_shot_ts;
      if (burst_duration_ms > 0) {
        rounds_per_second = (float)(shot_count - 1) * 1000.0 / burst_duration_ms;
      }
    } else {
      rounds_per_second = 0; // Not a burst, just a single shot
    }

    
    ws.textAll(packData(speed_in_meters, joules, rounds_per_second));
    last_rounds_per_second = rounds_per_second;
    
    noInterrupts();
    rps_shot_count = 0;
    rps_first_shot_ts = 0;
    rps_last_shot_ts = 0;
    interrupts();
  }
}

void loop() {
  dnsServer.processNextRequest();
  ws.cleanupClients();

  copySensorTimestamps();
  processShot();
  handleRps();
}
