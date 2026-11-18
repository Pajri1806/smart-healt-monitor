#include <Wire.h>
#include <LiquidCrystal.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <Adafruit_MLX90614.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ========================================
// 1. KONFIGURASI PIN
// ========================================

#define SDA_PIN 21
#define SCL_PIN 22
#define BUZZER_PIN 19

// false = buzzer aktif HIGH
// true  = buzzer aktif LOW
#define BUZZER_ACTIVE_LOW true

// LCD: RS, E, D4, D5, D6, D7
LiquidCrystal lcd(13, 14, 18, 23, 25, 26);

// ========================================
// KONFIGURASI BLUETOOTH LOW ENERGY
// ========================================

#define BLE_DEVICE_NAME "SMART_HEALTH_ESP32"

#define SERVICE_UUID \
"12345678-1234-1234-1234-1234567890ab"

#define CHARACTERISTIC_UUID \
"abcdefab-1234-1234-1234-abcdefabcdef"

BLEServer *bleServer = nullptr;
BLECharacteristic *bleCharacteristic = nullptr;

bool bleConnected = false;

// ========================================
// 2. OBJEK SENSOR
// ========================================

MAX30105 maxSensor;
Adafruit_MLX90614 mlx;

// ========================================
// 3. KONFIGURASI MAX30102
// ========================================

const int SAMPLE_BUFFER_SIZE = 100;

uint32_t irBuffer[SAMPLE_BUFFER_SIZE];
uint32_t redBuffer[SAMPLE_BUFFER_SIZE];

int32_t spo2 = 0;
int8_t validSPO2 = 0;

int32_t heartRate = 0;
int8_t validHeartRate = 0;

// ========================================
// 4. STATUS SENSOR
// ========================================

bool maxReady = false;
bool mlxReady = false;

bool fingerDetected = false;
bool suhuValid = false;

// ========================================
// 5. BATAS SUHU
// ========================================

// Ambang percobaan alarm suhu objek
const float BATAS_SUHU = 37.5;

// ========================================
// 6. VARIABEL HASIL
// ========================================

float suhuAmbient = NAN;
float suhuObjek = NAN;

unsigned long previousLCD = 0;
unsigned long previousSerial = 0;

int halamanLCD = 0;

// ========================================
// 7. FUNGSI KONTROL BUZZER
// ========================================

void kontrolBuzzer(bool alarm) {

  if (BUZZER_ACTIVE_LOW) {

    digitalWrite(
      BUZZER_PIN,
      alarm ? LOW : HIGH
    );

  } else {

    digitalWrite(
      BUZZER_PIN,
      alarm ? HIGH : LOW
    );
  }
}

// ========================================
// 8. SETUP
// ========================================

void setup() {

  Serial.begin(115200);
  setupBLE();

  // Inisialisasi buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  kontrolBuzzer(false);

  // Inisialisasi LCD
  lcd.begin(16, 2);

  lcd.setCursor(0, 0);
  lcd.print("SMART HEALTH");
  lcd.setCursor(0, 1);
  lcd.print("MONITOR ESP32");

  delay(2000);
  lcd.clear();

  // Inisialisasi I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  Serial.println("==========================");
  Serial.println("SMART HEALTH MONITOR");
  Serial.println("==========================");

  // Inisialisasi MLX90614
  Serial.println("Memeriksa MLX90614...");

  mlxReady = mlx.begin();

  if (mlxReady) {
    Serial.println("MLX90614 TERDETEKSI");
  } else {
    Serial.println("MLX90614 TIDAK TERDETEKSI");
  }

  // Inisialisasi MAX30102
  Serial.println("Memeriksa MAX30102...");

  maxReady = maxSensor.begin(
    Wire,
    I2C_SPEED_STANDARD
  );

  if (maxReady) {

    Serial.println("MAX30102 TERDETEKSI");

    byte ledBrightness = 60;
    byte sampleAverage = 4;
    byte ledMode = 2;
    byte sampleRate = 100;
    int pulseWidth = 411;
    int adcRange = 4096;

    maxSensor.setup(
      ledBrightness,
      sampleAverage,
      ledMode,
      sampleRate,
      pulseWidth,
      adcRange
    );

  } else {

    Serial.println("MAX30102 TIDAK TERDETEKSI");
  }

  // Tampilan status awal
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Status Sensor:");

  lcd.setCursor(0, 1);

  if (mlxReady && maxReady) {
    lcd.print("Semua Terhubung");
  } else {
    lcd.print("Cek Sensor!");
  }

  delay(2000);
  lcd.clear();
}

// ========================================
// CALLBACK KONEKSI BLE
// ========================================

class BLEConnectionCallbacks : public BLEServerCallbacks {

  void onConnect(BLEServer *server) override {
    bleConnected = true;
    Serial.println("ANDROID TERHUBUNG");
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    Serial.println("ANDROID TERPUTUS");

    BLEDevice::startAdvertising();
    Serial.println("BLE ADVERTISING ULANG");
  }
};

// ========================================
// INISIALISASI BLE
// ========================================

void setupBLE() {

  BLEDevice::init(BLE_DEVICE_NAME);

  bleServer = BLEDevice::createServer();

  bleServer->setCallbacks(
    new BLEConnectionCallbacks()
  );

  BLEService *service =
    bleServer->createService(SERVICE_UUID);

  bleCharacteristic =
    service->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY
    );

  bleCharacteristic->addDescriptor(
    new BLE2902()
  );

  bleCharacteristic->setValue(
    "{\"status\":\"waiting\"}"
  );

  service->start();

  BLEAdvertising *advertising =
    BLEDevice::getAdvertising();

  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.println("BLE SIAP");
  Serial.println(BLE_DEVICE_NAME);
}


// ========================================
// KIRIM DATA SENSOR KE ANDROID
// ========================================

void kirimDataBLE() {

  if (!bleConnected || bleCharacteristic == nullptr) {
    return;
  }

  bool bpmValid =
    maxReady &&
    fingerDetected &&
    validHeartRate &&
    heartRate > 0;

  bool spo2Valid =
    maxReady &&
    fingerDetected &&
    validSPO2 &&
    spo2 > 0 &&
    spo2 <= 100;

  String data = "{";

  data += "\"bpm\":";

  if (bpmValid) {
    data += String(heartRate);
  } else {
    data += "null";
  }

  data += ",\"spo2\":";

  if (spo2Valid) {
    data += String(spo2);
  } else {
    data += "null";
  }

  data += ",\"temperature\":";

  if (mlxReady && suhuValid) {
    data += String(suhuObjek, 2);
  } else {
    data += "null";
  }

  data += ",\"bpmValid\":";
  data += bpmValid ? "true" : "false";

  data += ",\"spo2Valid\":";
  data += spo2Valid ? "true" : "false";

  data += ",\"temperatureValid\":";
  data += (mlxReady && suhuValid) ? "true" : "false";

  data += "}";

  bleCharacteristic->setValue(data.c_str());
  bleCharacteristic->notify();

  Serial.print("BLE SEND: ");
  Serial.println(data);
}

// ========================================
// 9. LOOP UTAMA
// ========================================

void loop() {

  // ======================================
  // A. MEMBACA SUHU MLX90614
  // ======================================

  suhuValid = false;

  if (mlxReady) {

    suhuAmbient = mlx.readAmbientTempC();
    suhuObjek = mlx.readObjectTempC();

    if (
      !isnan(suhuAmbient) &&
      !isnan(suhuObjek) &&
      suhuAmbient > -40 &&
      suhuAmbient < 125 &&
      suhuObjek > -40 &&
      suhuObjek < 380
    ) {

      suhuValid = true;
    }
  }

  // ======================================
  // B. MEMBACA MAX30102
  // ======================================

  fingerDetected = false;

  if (maxReady) {

    for (int i = 0; i < SAMPLE_BUFFER_SIZE; i++) {

      while (!maxSensor.available()) {
        maxSensor.check();
      }

      redBuffer[i] = maxSensor.getRed();
      irBuffer[i] = maxSensor.getIR();

      maxSensor.nextSample();

      if (irBuffer[i] > 50000) {
        fingerDetected = true;
      }
    }

    // Periksa sinyal sampel terakhir
    if (irBuffer[SAMPLE_BUFFER_SIZE - 1] <= 50000) {
      fingerDetected = false;
    }

    if (fingerDetected) {

      maxim_heart_rate_and_oxygen_saturation(
        irBuffer,
        SAMPLE_BUFFER_SIZE,
        redBuffer,
        &spo2,
        &validSPO2,
        &heartRate,
        &validHeartRate
      );

    } else {

      validHeartRate = 0;
      validSPO2 = 0;
    }
  }

  // ======================================
  // C. KONTROL BUZZER
  // ======================================

  bool alarmSuhu = false;

  if (mlxReady && suhuValid) {

    if (suhuObjek > BATAS_SUHU) {
      alarmSuhu = true;
    }
  }

  kontrolBuzzer(alarmSuhu);

  // ======================================
  // D. SERIAL MONITOR
  // ======================================

  if (millis() - previousSerial >= 1000) {

    previousSerial = millis();

    Serial.println();
    Serial.println("===== HASIL PEMERIKSAAN =====");

    // SUHU
    if (suhuValid) {

      Serial.print("Suhu Ambient: ");
      Serial.print(suhuAmbient, 2);
      Serial.println(" C");

      Serial.print("Suhu Objek: ");
      Serial.print(suhuObjek, 2);
      Serial.println(" C");

      Serial.print("Status Suhu: ");

      if (alarmSuhu) {
        Serial.println("ALARM");
      } else {
        Serial.println("Normal");
      }

    } else {

      Serial.println("Suhu: DATA TIDAK VALID");
    }

    // BPM
    if (!maxReady) {

      Serial.println("BPM: SENSOR ERROR");

    } else if (!fingerDetected) {

      Serial.println("BPM: LETAKKAN JARI");

    } else if (validHeartRate && heartRate > 0) {

      Serial.print("BPM: ");
      Serial.println(heartRate);

    } else {

      Serial.println("BPM: BELUM VALID");
    }

    // SPO2
    if (!maxReady) {

      Serial.println("SpO2: SENSOR ERROR");

    } else if (!fingerDetected) {

      Serial.println("SpO2: LETAKKAN JARI");

    } else if (
      validSPO2 &&
      spo2 > 0 &&
      spo2 <= 100
    ) {

      Serial.print("SpO2: ");
      Serial.print(spo2);
      Serial.println(" %");

    } else {

      Serial.println("SpO2: BELUM VALID");
    }

    // BUZZER
    Serial.print("Buzzer: ");
    Serial.println(alarmSuhu ? "AKTIF" : "MATI");

    Serial.println("=============================");
  }

  // ======================================
  // E. LCD 16x2
  // ======================================

  if (millis() - previousLCD >= 2500) {

    previousLCD = millis();

    lcd.clear();

    if (halamanLCD == 0) {

      // HALAMAN 1: SUHU DAN BPM

      lcd.setCursor(0, 0);
      lcd.print("Suhu:");

      if (suhuValid) {

        lcd.print(suhuObjek, 1);
        lcd.write((uint8_t)223);
        lcd.print("C");

      } else {

        lcd.print("Error");
      }

      lcd.setCursor(0, 1);
      lcd.print("BPM:");

      if (!maxReady) {

        lcd.print("Sensor Error");

      } else if (!fingerDetected) {

        lcd.print("Letak Jari");

      } else if (validHeartRate && heartRate > 0) {

        lcd.print(heartRate);

      } else {

        lcd.print("Belum Valid");
      }

    } else {

      // HALAMAN 2: SPO2 DAN STATUS

      lcd.setCursor(0, 0);
      lcd.print("SpO2:");

      if (!maxReady) {

        lcd.print("Sensor Error");

      } else if (!fingerDetected) {

        lcd.print("Letak Jari");

      } else if (
        validSPO2 &&
        spo2 > 0 &&
        spo2 <= 100
      ) {

        lcd.print(spo2);
        lcd.print("%");

      } else {

        lcd.print("Belum Valid");
      }

      lcd.setCursor(0, 1);

      if (!mlxReady || !maxReady) {

        lcd.print("Cek Koneksi");

      } else if (!suhuValid) {

        lcd.print("Suhu Error");

      } else if (alarmSuhu) {

        lcd.print("Alarm Suhu!");

      } else {

        lcd.print("Sistem Aktif");
      }
    }

    halamanLCD = 1 - halamanLCD;
  }
  
  // ======================================
// F. PENGIRIMAN DATA BLE
// ======================================

static unsigned long previousBLE = 0;

if (millis() - previousBLE >= 1000) {

  previousBLE = millis();

  kirimDataBLE();
}
}
