#define FIRSTSENSOR 1
#define SECONDSENSOR 3
#define DISTANCEACROSS 70.0
#define TIMESTAMPBUFFERSIZE 10

#include <Arduino_JSON.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include "LittleFS.h"

#include <AsyncElegantOTA.h>
#include <WebSerial.h>
const byte DNS_PORT = 53;
DNSServer dnsServer;
AsyncWebServer webServer(80);
AsyncWebSocket ws("/ws");

class CaptiveRequestHandler : public AsyncWebHandler {
public:
  virtual ~CaptiveRequestHandler() {}
  CaptiveRequestHandler() {
    WebSerial.begin(&webServer);
    AsyncElegantOTA.begin(&webServer);
  }


  bool canHandle(AsyncWebServerRequest *request) {
    String url = request->url();
    return (url == "/generate_204" || url == "/hotspot-detect.html");
  }

  void handleRequest(AsyncWebServerRequest *request) {
    if (!request) {
      Serial.println("Received null request");
      return;  // Return early if the request is null
    }
    // Check for specific requests (e.g., Android's `/generate_204`)
    if (request->url() == "/generate_204" || request->url() == "/hotspot-detect.html") {
      // Redirect to the captive portal (ESP8266’s IP)
      request->redirect("http://192.168.5.5/index.html");
    }
  }
};
void initPins() {
  pinMode(FIRSTSENSOR, INPUT);
  pinMode(SECONDSENSOR, INPUT);
  Serial.println("Sensors Initialized");
}

void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("An error has occurred while mounting LittleFS");
  }
  Serial.println("LittleFS mounted successfully");
}

void initWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 5, 5), IPAddress(192, 168, 5, 5), IPAddress(255, 255, 255, 0));
  WiFi.softAP("Chronograph");
  Serial.println("WiFi Initialized");
}

void initCaptivePortal() {
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 5, 5));
  webServer.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
  webServer.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  ws.onEvent(onEvent);
  webServer.addHandler(&ws);
  webServer.begin();
  Serial.println("Captive Portal Initialized");
}

void initInterrupts() {
  attachInterrupt(digitalPinToInterrupt(FIRSTSENSOR), get_first_sensor, FALLING);
  attachInterrupt(digitalPinToInterrupt(SECONDSENSOR), get_second_sensor, FALLING);
  Serial.println("Interrupts Initialized");
}

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char *)data;
    // Check if the message is "sendValues"
    if (strcmp((char *)data, "sendValues") == 0) {
      ws.textAll(packData(random(0,100),random(0,20)*1.0/10,random(0,45)));
    }
  }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
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
  initCaptivePortal();
  initInterrupts();
}

unsigned long timeStampFirst[TIMESTAMPBUFFERSIZE] = { 0 }, timeStampSecond[TIMESTAMPBUFFERSIZE] = { 0 };
int indexesFirst[2] = { 0 }, indexesSecond[2] = { 0 };
//var for speed measurement
double bbWeight = 0.28;
//var for rps measurement
float lastShot = 0;
unsigned long sumOfTimeBetweenShots = 0;
int numberOfShots = 0;

void get_first_sensor() {
  if(timeStampFirst[indexesFirst[1]] != 0) timeStampFirst[indexesFirst[1]++] = millis();
  if(indexesFirst[1] >= TIMESTAMPBUFFERSIZE) indexesFirst[1] = 0;
}
void get_second_sensor() {
  if(timeStampSecond[indexesSecond[1]] != 0) timeStampSecond[indexesSecond[1]++] = millis();
  if(indexesSecond[1] >= TIMESTAMPBUFFERSIZE) indexesSecond[1]=0;
}

String packData(float metric, float joules, float rps) {
  JSONVar data;
  char v1[20],v2[20],v3[20];
  dtostrf(metric, 1, 2, v1);
  dtostrf(joules, 1, 2, v2);
  dtostrf(rps, 1, 2, v3);
  data["metric"]=v1;
  data["joules"]=v2;
  data["rps"]=v3;
  return JSON.stringify(data);
}

void resetRPSValues() {
  sumOfTimeBetweenShots = 0;
  numberOfShots = 0;
}

void processPairOfTimeStamps() {
  float metric = (DISTANCEACROSS / 1000.0) / ((timeStampSecond[indexesSecond[0]] - timeStampFirst[indexesFirst[0]]) / 1000);
  float joules = ((bbWeight / 1000.0) / 2) * metric * metric;
  if(timeStampFirst[indexesFirst[0]] - lastShot < 1000)
    sumOfTimeBetweenShots = sumOfTimeBetweenShots + (timeStampFirst[indexesFirst[0]] - lastShot);
  else
    resetRPSValues();
  lastShot = timeStampFirst[indexesFirst[0]];
  numberOfShots++;
  float rps = 1.0/((sumOfTimeBetweenShots*1.0)/(numberOfShots))+1;
  ws.textAll(packData(metric, joules, rps));
  timeStampFirst[indexesFirst[0]++]=0;
  if(indexesFirst[0]>=TIMESTAMPBUFFERSIZE) indexesFirst[0]=0;
  timeStampSecond[indexesSecond[0]++]=0;
  if(indexesSecond[0]>=TIMESTAMPBUFFERSIZE) indexesSecond[0]=0;
}

void loop() {
  dnsServer.processNextRequest();

  if (timeStampFirst[indexesFirst[0]]!=0 && timeStampSecond[indexesSecond[0]]!=0)
    processPairOfTimeStamps();

  ws.cleanupClients();
}
