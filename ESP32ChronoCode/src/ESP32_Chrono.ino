#define FIRSTSENSOR 10
#define SECONDSENSOR 7

#define TIMESTAMP_QUEUE_SIZE 16 // Can handle up to 16 BBs in flight between sensors
#define BURST_TIMEOUT_MS 1000 // ms to wait after the last shot to finalize RPS calculation
#define DEBOUNCE_TIME_US 200   // 2us debounce

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

// Circular queue for timestamps from the first sensor (in microseconds)
volatile unsigned long first_sensor_ts_us[TIMESTAMP_QUEUE_SIZE];//timestamps
volatile int ts_queue_head = 0;
volatile int ts_queue_tail = 0;

// Circular queue for completed shot travel times (in microseconds)
volatile unsigned long completed_shot_t_us[TIMESTAMP_QUEUE_SIZE];//time between sensors
volatile int completed_queue_head = 0;
volatile int completed_queue_tail = 0;

// Volatile variables for RPS calculation, modified by ISR
volatile int shot_count = 0;
volatile unsigned long first_shot_ts_us = 0;
volatile unsigned long last_shot_ts_us = 0;

// Debouncing variables
volatile unsigned long last_ts_entry_us = 0;
volatile unsigned long last_ts_exit_us = 0;

// Settings
float DISTANCEACROSS = 67.75;
float BBWEIGHT = 0.28;
// End Settings



void IRAM_ATTR getFirstSensor() {
  unsigned long now_us = micros();

  if (now_us - last_ts_entry_us < DEBOUNCE_TIME_US) return;

  last_ts_entry_us = now_us;

  int next_head = (ts_queue_head + 1) % TIMESTAMP_QUEUE_SIZE;
  if (next_head == ts_queue_tail) return; // Queue is full, drop shot

  first_sensor_ts_us[ts_queue_head] = now_us;
  ts_queue_head = next_head;
}

void IRAM_ATTR getSecondSensor() {
  unsigned long now_us = micros();

  if (now_us - last_ts_exit_us < DEBOUNCE_TIME_US) return;

  last_ts_exit_us = now_us;

  if (ts_queue_head == ts_queue_tail) return; // Spurious trigger

  unsigned long first_ts = first_sensor_ts_us[ts_queue_tail];
  ts_queue_tail = (ts_queue_tail + 1) % TIMESTAMP_QUEUE_SIZE;

  unsigned long travel_time_us = now_us - first_ts;
  
  int next_completed_head = (completed_queue_head + 1) % TIMESTAMP_QUEUE_SIZE;
  if (next_completed_head == completed_queue_tail) return; // Completed queue full
  
  completed_shot_t_us[completed_queue_head] = travel_time_us;
  completed_queue_head = next_completed_head;

  if (shot_count == 0) first_shot_ts_us = now_us;
  last_shot_ts_us = now_us;
  shot_count++;
}

// --- End of Chronograph Logic ---

void initPins() {
  pinMode(FIRSTSENSOR, INPUT);
  pinMode(SECONDSENSOR, INPUT);
  Serial.println("Sensors Initialized with internal pull-ups");
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
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/index.html", "text/html");
  });
  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/style.css", "text/css");
  });
  webServer.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/script.js", "application/javascript");
  });
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
  attachInterrupt(digitalPinToInterrupt(FIRSTSENSOR), getFirstSensor, RISING);
  attachInterrupt(digitalPinToInterrupt(SECONDSENSOR), getSecondSensor, RISING);
  Serial.println("Interrupts Initialized");
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
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
    case WS_EVT_CONNECT:{
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      JSONVar settings;
      settings["bbWeight"] = BBWEIGHT;
      settings["distanceAcross"] = DISTANCEACROSS;
      client->text(JSON.stringify(settings));
      break;}
    case WS_EVT_DISCONNECT:{
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;}
    case WS_EVT_DATA:{
      handleWebSocketMessage(arg, data, len);
      break;}
    case WS_EVT_PONG: {break;}
    case WS_EVT_ERROR:{break;}
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

String packData(const float* speed, const float* energy, const float* rps) {
  JSONVar data;
  if (speed) data["metric"] = *speed;
  if (energy) data["joules"] = *energy;
  if (rps) data["rps"] = *rps;
  return JSON.stringify(data);
}

void handleSingleShot(unsigned long travel_time_us) {
    float speed = 0, energy = 0;
    float travel_time_s = (travel_time_us * 1.0) / 1000000.0;

    if (travel_time_s > 0) {
        speed = (DISTANCEACROSS / 1000.0) / travel_time_s;
        energy = (BBWEIGHT / 2000.0) * speed * speed;
    }
    ws.textAll(packData(&speed, &energy, nullptr));
}

void handleEndOfBurst() {

    noInterrupts();
    unsigned long burst_duration_us = last_shot_ts_us - first_shot_ts_us;
    interrupts();

    if ((completed_queue_head == completed_queue_tail)) {
        float rounds_per_second = 0;
        
        if (burst_duration_us > 0) {
            rounds_per_second = (float)(shot_count - 1) * 1000000.0 / burst_duration_us;
        }
        ws.textAll(packData(nullptr, nullptr, &rounds_per_second));
    }
}

void processData() {
    if (completed_queue_head != completed_queue_tail) {
        unsigned long travel_time_us;

        if(micros() - last_shot_ts_us > BURST_TIMEOUT_MS * 1000ULL){
          noInterrupts();
          travel_time_us = completed_shot_t_us[completed_queue_tail];
          completed_queue_tail = (completed_queue_tail + 1) % TIMESTAMP_QUEUE_SIZE;
          interrupts();
          if(shot_count==1){
            handleSingleShot(travel_time_us);
          }
          else{
            handleEndOfBurst();
          }
          shot_count = 0;
        }
    }

}

void loop() {
  dnsServer.processNextRequest();
  ws.cleanupClients();
  processData();
}

