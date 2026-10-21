#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>

#include <esp_pm.h>

#define SERVICE_UUID        "4fa8f700-4323-40f9-a961-68f1488d8a1e"
#define CHAR_SHOT_UUID      "4fa8f701-4323-40f9-a961-68f1488d8a1e"

#define FIRSTSENSOR 10
#define SECONDSENSOR 7
#define DEBUGPIN 6

#define TIMESTAMP_QUEUE_SIZE 16 // Can handle up to 16 BBs in flight between sensors
#define RPS_TIMEOUT_US 1000000 // ms to wait after the last shot to finalize RPS calculation

#define SSID "Chronograph"
#define PASSWORD "Chrono123"

WebServer ota_server(80);
bool ota_mode = false;

struct circular_buffer{
    unsigned long values[TIMESTAMP_QUEUE_SIZE];
    int head = 0;
    int tail = 0;
};

// --- Data buffers
volatile circular_buffer first_sensor_timestamps;
volatile circular_buffer delta_timestamps;
// ---

// --- RPS related
volatile int shot_count = 0;
volatile unsigned long rps_start;
volatile unsigned long rps_end;
float latest_rps_reading = 0;
// ---

BLECharacteristic *p_shot_char = nullptr;
bool device_connected = false;

class ServerCallbacks: public BLEServerCallbacks{
    void onConnect(BLEServer* p_server) override{
        device_connected = true;
        BLEDevice::stopAdvertising();
    }

    void onDisconnect(BLEServer* p_server) override{
        device_connected = false;
        p_server->startAdvertising();
    }
};

void configAutoLightSleep(){
    esp_pm_config_esp32c3_t pwr_config = {
        .max_freq_mhz = 80,
        .min_freq_mhz = 10,
        .light_sleep_enable = true
    };
}

void IRAM_ATTR firstSensorTrigger() {
    unsigned long now_us = micros();

    if (shot_count == 0) rps_start = now_us;
    rps_end = now_us;
    shot_count++;

    int next_head = (first_sensor_timestamps.head + 1) & (TIMESTAMP_QUEUE_SIZE-1);
    if (next_head == first_sensor_timestamps.tail) return; // Buffer full

    first_sensor_timestamps.values[first_sensor_timestamps.head] = now_us;
    first_sensor_timestamps.head = next_head;
}

void IRAM_ATTR secondSensorTrigger() {
    unsigned long now_us = micros();

    if(first_sensor_timestamps.head == first_sensor_timestamps.tail) return; // Probably a bad read

    unsigned long travel_time_us = now_us - first_sensor_timestamps.values[first_sensor_timestamps.tail];
    first_sensor_timestamps.tail = (first_sensor_timestamps.tail + 1) & (TIMESTAMP_QUEUE_SIZE-1);

    int next_head = (delta_timestamps.head + 1) & (TIMESTAMP_QUEUE_SIZE-1);
    if (next_head == delta_timestamps.tail) return; // Buffer full
  
    delta_timestamps.values[delta_timestamps.head] = travel_time_us;
    delta_timestamps.head = next_head;
}

void initBLE(){
    BLEDevice::init("ESP32-Chronograph");

    BLEDevice::setMTU(512);

    BLEServer *p_server = BLEDevice::createServer();
    p_server->setCallbacks(new ServerCallbacks());

    BLEService *p_service = p_server->createService(SERVICE_UUID);
    p_shot_char = p_service->createCharacteristic(
        CHAR_SHOT_UUID,
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    p_shot_char->addDescriptor(new BLE2902);
    p_service->start();

    BLEAdvertising *p_advertising = BLEDevice::getAdvertising();
    p_advertising->addServiceUUID(SERVICE_UUID);

    p_advertising->setScanResponse(true);
    p_advertising->setMinPreferred(0x0100);
    p_advertising->setMaxPreferred(0x0200);

    p_server->getAdvertising()->start();
}

void initOTA(){
    ota_mode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(SSID,PASSWORD);

    delay(100);

    ota_server.on("/" , HTTP_GET , [](){
        ota_server.send(200 , "text/plain" , "OTA mode Active");
    });

    ElegantOTA.begin(&ota_server);
    ota_server.begin();
}


void initPins() {
    pinMode(DEBUGPIN , INPUT_PULLUP);
    pinMode(FIRSTSENSOR, INPUT);
    pinMode(SECONDSENSOR, INPUT);
}

void initInterrupts() {
    attachInterrupt(digitalPinToInterrupt(FIRSTSENSOR), firstSensorTrigger, RISING);
    attachInterrupt(digitalPinToInterrupt(SECONDSENSOR), secondSensorTrigger, RISING);
}

void setup(){
    initPins();

    if(digitalRead(DEBUGPIN) == LOW){
       initOTA();
       return; // Entered upload mode , ignore rest of inits
    }

    initInterrupts();
    initBLE();
    configAutoLightSleep();
}

void sendProcessedData(){
    //send speed and rps data to android app
    unsigned long projectile_delta_time = delta_timestamps.values[delta_timestamps.tail];

    if(projectile_delta_time == 0) return;

    float projectile_speed = 60000.0f / (float)projectile_delta_time ; 
    delta_timestamps.tail = (delta_timestamps.tail + 1) & (TIMESTAMP_QUEUE_SIZE - 1);

    if(device_connected && p_shot_char != nullptr)  {
        uint8_t packet[4];
        uint16_t speed_dms = (uint16_t)(projectile_speed * 10);
        uint16_t rps_tenths = (uint16_t)(latest_rps_reading * 10);

        packet[0] = (uint8_t)(speed_dms >> 8);
        packet[1] = (uint8_t)(speed_dms & 0xFF);
        packet[2] = (uint8_t)(rps_tenths >> 8);
        packet[3] = (uint8_t)(rps_tenths & 0xFF);

        p_shot_char->setValue(packet, 4);
        p_shot_char->notify();
    }
}

void processRPS(){
    unsigned long rps_delta = rps_end - rps_start;
    rps_start=0;
    rps_end=0;

    if(rps_delta == 0) return; // Single shot condition

    latest_rps_reading = ((shot_count-1)*1.0)/((rps_delta*1.0)/1000000);

    shot_count = 0;
}

void loop(){
    if(ota_mode){
        ota_server.handleClient();
        ElegantOTA.loop();
        return;
    }

    unsigned long rps_end_cache = rps_end;

    if((micros()-rps_end_cache > RPS_TIMEOUT_US) && rps_end_cache != 0){
        processRPS();
    }

    if(delta_timestamps.head!=delta_timestamps.tail && device_connected){
        sendProcessedData();
    }

    delay(50);
}