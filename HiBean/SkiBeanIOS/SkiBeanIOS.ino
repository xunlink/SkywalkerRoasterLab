/***************************************************
 * HiBean ESP32 BLE Roaster Control
 * 
 * Replaces Classic Bluetooth with BLE (NUS).
 * 
 * Service UUID:
 *     6e400001-b5a3-f393-e0a9-e50e24dcca9e
 * Characteristics UUIDs:
 *   - Write Characteristic (RX):
 *     6e400002-b5a3-f393-e0a9-e50e24dcca9e
 *   - Notify Characteristic (TX):
 *     6e400003-b5a3-f393-e0a9-e50e24dcca9e
 *
 * Sends notifications for temperature/status data.
 * Expects commands via the write characteristic.
 *
 * Library credit: MedianFilter
 * https://github.com/luisllamasbinaburo/Arduino-MedianFilter
 ***************************************************/

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "MedianFilterLib.h"

// -----------------------------------------------------------------------------
// Set SERIAL_DEBUG to 1 to enable serial output
// Trying to print AND roaster rx/tx is not compatible with single UART boards (ie. s3-zero)
// -----------------------------------------------------------------------------
#define SERIAL_DEBUG 0 //set to 1 to turn on

// -----------------------------------------------------------------------------
// Macro redefines of Serial.print for debug on/off
// -----------------------------------------------------------------------------
#if SERIAL_DEBUG == 1
#define D_print(...)    Serial.print(__VA_ARGS__)
#define D_println(...)  Serial.println(__VA_ARGS__)
#else
#define D_print(...)
#define D_println(...)
#endif

// -----------------------------------------------------------------------------
// Pin Definitions
// -----------------------------------------------------------------------------
#if SERIAL_DEBUG == 1
  const int TX_PIN = NULL;  // bogus pin
  const int RX_PIN = NULL;  // bogus pin
  const int LED_PIN = NULL; // bogus pin
#elif defined(ARDUINO_WAVESHARE_ESP32_S3_ZERO)
  const int TX_PIN = 19;  // Output pin to roaster
  const int RX_PIN = 20;  // Input pin from roaster
  const int LED_PIN = 21; // WaveShare S3 on-board LED
#else
  const int TX_PIN = 1;  // bogus pin
  const int RX_PIN = 2;  // bogus pin
  const int LED_PIN = 0; // bogus pin
#endif

// -----------------------------------------------------------------------------
// BLE UUIDs for Nordic UART Service
// -----------------------------------------------------------------------------
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e" // NUS service
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // Write
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // Notify

// -----------------------------------------------------------------------------
// BLE Globals
// -----------------------------------------------------------------------------
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
bool deviceConnected = false;


// -----------------------------------------------------------------------------
// Timing Constants
// -----------------------------------------------------------------------------
const int PREAMBLE        = 7000;
const int PULSE_ONE       = 1200;
const int PULSE_ZERO      = 650;
const int POST_PULSE_DELAY= 750;
const int START_PULSE     = 7500;
const int START_DELAY     = 3800;

// -----------------------------------------------------------------------------
// Buffer Sizes
// -----------------------------------------------------------------------------
const int ROASTER_LENGTH    = 7;   // 7 bytes received from roaster
const int CONTROLLER_LENGTH = 6;   // 6 bytes sent to roaster

// -----------------------------------------------------------------------------
// Allocate buffers
// -----------------------------------------------------------------------------
uint8_t receiveBuffer[ROASTER_LENGTH];
uint8_t sendBuffer[CONTROLLER_LENGTH];

// Raw temperature values
uint16_t rawTempX, rawTempY;

// -----------------------------------------------------------------------------
// Control Byte Indices
// -----------------------------------------------------------------------------
enum ControlBytes {
    VENT_BYTE = 0,
    DRUM_BYTE = 3,
    COOL_BYTE = 2,
    FILTER_BYTE = 1,
    HEAT_BYTE = 4,
    CHECK_BYTE = 5
};

// Control Bytes & Checksum
void setControlChecksum() {
    uint8_t sum = 0;
    for (int i = 0; i < (CONTROLLER_LENGTH - 1); i++) {
        sum += sendBuffer[i];
    }
    sendBuffer[CHECK_BYTE] = sum;  // Correct use of CHECK_BYTE
}

void setValue(uint8_t* bytePtr, uint8_t value) {
    *bytePtr = value;
    setControlChecksum();
}

// -----------------------------------------------------------------------------
// Temperature Variables & Filter
// -----------------------------------------------------------------------------
double temp          = 0.0;           // Filtered temperature
unsigned long lastEventTime = 0;
const unsigned long LAST_EVENT_TIMEOUT = 10UL * 1000000UL; // 10 seconds (micros)
char CorF            = 'C';           // 'C' or 'F'
int filtWeight       = 80;            // Filter weight (Artisan default)

// -----------------------------------------------------------------------------
// Command Strings
// -----------------------------------------------------------------------------
const String CMD_READ         = "READ";
const String CMD_HEAT         = "OT1";
const String CMD_VENT         = "OT2";
const String CMD_OFF          = "OFF";
const String CMD_DRUM         = "DRUM";
const String CMD_FILTER       = "FILTER";
const String CMD_COOL         = "COOL";
const String CMD_CHAN         = "CHAN";
const String CMD_UNITS        = "UNITS";
const String CMD_FILTER_WEIGHT= "FILT";

// -----------------------------------------------------------------------------
// Forward Declarations
// -----------------------------------------------------------------------------
void parseAndExecuteCommands(String input);
void executeCommand(String command, uint8_t value);
void shutdown();
void sendMessage();
void getRoasterMessage();

// -----------------------------------------------------------------------------
// BLE Server Callbacks
// -----------------------------------------------------------------------------
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t *param) override {
    deviceConnected = true;

    // Change BLE connection parameters per apple ble guidelines
    // (for this client, min interval 15ms (/1.25), max 30ms (/1.25), latency 4 frames, timeout 5sec(/10ms)
    // https://docs.silabs.com/bluetooth/4.0/bluetooth-miscellaneous-mobile/selecting-suitable-connection-parameters-for-apple-devices
    pServer->updateConnParams(param->connect.remote_bda, 12, 24, 4, 500);
   
    D_println("BLE: Client connected.");
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    D_println("BLE: Client disconnected. Restarting advertising...");
    pServer->getAdvertising()->start();
  }
};

// -----------------------------------------------------------------------------
// BLE Characteristic Callbacks
// -----------------------------------------------------------------------------
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String rxValue = String(pCharacteristic->getValue().c_str());

    if (rxValue.length() > 0) {
      String input = String(rxValue.c_str());
      D_print("BLE Write Received: ");
      D_println(input);
      parseAndExecuteCommands(input);
    }
  }
};

// -----------------------------------------------------------------------------
// Utility Functions
// -----------------------------------------------------------------------------
bool itsbeentoolong() {
  unsigned long now = micros();
  unsigned long duration = now - lastEventTime;
  return (duration > LAST_EVENT_TIMEOUT);
}

void notifyClient(const String& message) {
    D_println("Attempting to notify BLE client with: " + message);

    if (deviceConnected && pTxCharacteristic) {
        pTxCharacteristic->setValue(message.c_str());
        pTxCharacteristic->notify();
        D_println("Notification sent successfully.");
    } else {
      D_println("Notification failed. Device not connected or TX characteristic unavailable.");
    }
}

void shutdown() {
    for (int i = 0; i < CONTROLLER_LENGTH; i++) {
        sendBuffer[i] = 0;
    }
    sendBuffer[CHECK_BYTE] = 0; // Reset checksum
}

void sendMessage() {
    // Start pulse
    pulsePin(TX_PIN, START_PULSE);
    delayMicroseconds(START_DELAY);

    // Send each byte, bit by bit
    for (int i = 0; i < CONTROLLER_LENGTH; i++) {
        for (int j = 0; j < 8; j++) {
            if (bitRead(sendBuffer[i], j) == 1) {
                // '1' bit
                pulsePin(TX_PIN, 1500);
            } else {
                // '0' bit
                pulsePin(TX_PIN, PULSE_ZERO);
            }
            delayMicroseconds(POST_PULSE_DELAY);
        }
    }
}

void pulsePin(int pin, int duration) {
    digitalWrite(pin, LOW);
    delayMicroseconds(duration);
    digitalWrite(pin, HIGH);
}

// -----------------------------------------------------------------------------
// Interrupt to watch for start of roaster message
// https://forum.arduino.cc/t/detecting-pulses-of-certain-lengths-using-interrupts/360570/12 
// -----------------------------------------------------------------------------
unsigned long lastPulse;
volatile bool roasterStartFound = 0;

void watchRoasterStart() {
  unsigned long now = micros();
  
  if ((now - lastPulse) >= PREAMBLE) {
    roasterStartFound = 1;
  } else {
    roasterStartFound = 0;
  }
  lastPulse = now;
}

void getRoasterMessage() {
    bool passedChecksum = false;
    getMessage(ROASTER_LENGTH, RX_PIN);

    if ( calculateRoasterChecksum() ) {
      // Valid checksum, compute temperature with filtering
      filtTemp(calculateTemp());
    } else {
      D_println("Not valid roaster message.");
    }
}

void getMessage(int bytes, int pin) {
    unsigned long timeIntervals[ROASTER_LENGTH * 8];
    unsigned long pulseDuration = 0;
    int bits = bytes * 8;

    // Read bits
    for (int i = 0; i < bits; i++) {
      timeIntervals[i] = pulseIn(pin, LOW);
    }

    // Clear receiveBuffer
    for (int i = 0; i < bytes; i++) {
      receiveBuffer[i] = 0;
    }

    // Convert intervals into bits
    for (int i = 0; i < bits; i++) {
      if (timeIntervals[i] > PULSE_ONE) {
        receiveBuffer[i / 8] |= (1 << (i % 8));
      }
    }
}

bool calculateRoasterChecksum() {
    uint8_t sum = 0;
    for (int i = 0; i < (ROASTER_LENGTH - 1); i++) {
        sum += receiveBuffer[i];
    }
    return (sum == receiveBuffer[ROASTER_LENGTH - 1]);
}

double calculateTemp() {
    rawTempX = ((receiveBuffer[0] << 8) + receiveBuffer[1]);
    rawTempY = ((receiveBuffer[2] << 8) + receiveBuffer[3]);

    double x = 0.001 * rawTempX;
    double y = 0.001 * rawTempY;
    double v;  // Declare v here

    if (rawTempX > 836 || rawTempY > 221) {
        v = -224.2 * y * y * y + 385.9 * y * y - 327.1 * y + 171;
    } else {
        v = -278.33 * x * x * x + 491.944 * x * x - 451.444 * x + 310.668;
    }

    if (CorF == 'F') {
        v = 1.8 * v + 32.0;
    }
    return v;
}

MedianFilter<double> medianFilter(5);
void filtTemp(double v){
  if(v < 0 && v >= 300) { return; } //don't process blatantly bogus values
  medianFilter.AddValue(v);
  temp = medianFilter.GetFiltered();
}

void handleCHAN() {
    String message = "# Active channels set to 2100\r\n";
    D_println(message);
    notifyClient(message);
}

void handleREAD() {
    String output = "0," + String(temp, 1) + "," + String(temp, 1) + "," +
                    String(sendBuffer[HEAT_BYTE]) + "," +
                    String(sendBuffer[VENT_BYTE]) + "\r\n";

    D_print("READ Output: ");
    D_println(output);

    notifyClient(output);
    lastEventTime = micros();
}

void handleHEAT(uint8_t value) {
    if (value <= 100) {
        setValue(&sendBuffer[HEAT_BYTE], value);
    }
    lastEventTime = micros();
}

void handleVENT(uint8_t value) {
    if (value <= 100) {
        setValue(&sendBuffer[VENT_BYTE], value);
        if (value == 0) {
            handleFILTER(value); // off
        } else {
            handleFILTER((int) round(4-((value-1)*4/100))); //convert 0-100 to inverted 4-1
        }
    }
    lastEventTime = micros();
}

void handleDRUM(uint8_t value) {
    if (value != 0) {
        setValue(&sendBuffer[DRUM_BYTE], 100);
    } else {
        setValue(&sendBuffer[DRUM_BYTE], 0);
    }
    lastEventTime = micros();
}

void handleFILTER(uint8_t value) {
    if (value >= 0 && value <= 4 ) {
        setValue(&sendBuffer[FILTER_BYTE], value); //0 off; 1 fastest -> 4 slowest
    }
    lastEventTime = micros();
}

void handleCOOL(uint8_t value) {
    if (value <= 100) {
        setValue(&sendBuffer[COOL_BYTE], value);
        handleFILTER(value);
    }
    lastEventTime = micros();
}

void parseAndExecuteCommands(String input) {
    input.trim();
 
    D_println("Parsing command: " + input);

    while (input.length() > 0) {
        int split = input.indexOf(';');
        String command;
        String param;
        uint8_t value = 0;

        if (split >= 0) {
            command = input.substring(0, split);
            param = input.substring(split + 1);
            param.trim();

            if (param.length() > 0) {
                value = param.toInt();
            }

            input = "";
        } else {
            command = input;
            input = "";
        }

        if (command.indexOf("READ") != -1) {
            command = CMD_READ;
        }
        D_println("Parsed command: " + command + ", Value: " + String(value));

        executeCommand(command, value);
    }
}

void executeCommand(String command, uint8_t value) {
    if (command == CMD_READ) {
        handleREAD();
    } else if (command == CMD_HEAT) {
        handleHEAT(value);
    } else if (command == CMD_VENT) {
        handleVENT(value);
    } else if (command == CMD_OFF) {
        shutdown();
    } else if (command == CMD_DRUM) {
        handleDRUM(value);
    } else if (command == CMD_FILTER) {
        handleFILTER(value);
    } else if (command == CMD_COOL) {
        handleCOOL(value);
    } else if (command == CMD_CHAN) {
        handleCHAN();
    } else if (command == CMD_UNITS) {
        if (value != 0) {
            CorF = (char)value;
        }
    } else if (command.startsWith(CMD_FILTER_WEIGHT)) {
        int w = command.substring(4).toInt();
        if (w >= 0 && w <= 100) {
            filtWeight = w;
        }
        String message = "# Physical channel 1 filter set to " + String(filtWeight);
        
          D_println(message);

        notifyClient(message);
    }
}


// -----------------------------------------------------------------------------
// LED handler
// -----------------------------------------------------------------------------

const unsigned long LED_FLASH_DELAY_MS = 1000;
const unsigned int LED_BLUE[3] = { 0, 0, 10 };
const unsigned int LED_RED[3] = { 0, 10, 0 };
const unsigned int LED_GREEN[3] = { 10, 0, 0 };
const unsigned int LED_BLACK[3] = { 0, 0, 0 };

char* currentLEDColor = "blue";
unsigned long LED_LAST_ON_MS = 0;

// alternates blue-red on boot when no client connected
// when client conncted just flashes blue
void handleLED() {
  unsigned long t_now = millis();
  // flash led depending on state
  if( (t_now - LED_LAST_ON_MS) >= LED_FLASH_DELAY_MS ) {
    if(currentLEDColor == "blue" && deviceConnected) {
      rgbLedWrite(LED_PIN, LED_BLACK[0], LED_BLACK[1], LED_BLACK[2]);
      currentLEDColor = "black";
    } else if (currentLEDColor == "blue") {
      rgbLedWrite(LED_PIN, LED_RED[0], LED_RED[1], LED_RED[2]);
      currentLEDColor = "red";
      LED_LAST_ON_MS = t_now;
    } else {
      rgbLedWrite(LED_PIN, LED_BLUE[0], LED_BLUE[1], LED_BLUE[2]);
      currentLEDColor = "blue";
      LED_LAST_ON_MS = t_now;
    }
  }
}

void setup() {
    rgbLedWrite(LED_PIN, LED_GREEN[0], LED_GREEN[1], LED_GREEN[2]);
    Serial.begin(115200);
    Serial.println("Starting HiBean ESP32 BLE Roaster Control.");
    delay(3000); //let fw upload finish before we take over hwcdc serial tx/rx

    D_println("Serial SERIAL_DEBUG ON!");
    
    #if SERIAL_DEBUG == 0
    pinMode(TX_PIN, OUTPUT);
    digitalWrite(TX_PIN, HIGH);
    pinMode(RX_PIN, INPUT);
    #endif

    BLEDevice::init("ESP32_Skycommand_BLE");
    BLEDevice::setMTU(185);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    BLECharacteristic* pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
    );
    pRxCharacteristic->setCallbacks(new MyCallbacks());
    pRxCharacteristic->addDescriptor(new BLE2902());

    pService->start();

    BLEAdvertising* pAdvertising = pServer->getAdvertising();
    pAdvertising->start();

    D_println("BLE Advertising started...");

    // interrupt that flags when roaster preamble is found
    attachInterrupt(RX_PIN, watchRoasterStart, FALLING);

    shutdown();
}

void loop() {
    // roaster shut down, clear our buffers   
    if (itsbeentoolong()) { shutdown(); }

    // roaster message start found, go get it
    if (roasterStartFound) { getRoasterMessage(); }

    // send roaster commands if any
    sendMessage();

    // update the led so user knows we're running
    handleLED();
}