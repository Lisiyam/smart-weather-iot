#include <Arduino.h>

#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <time.h>  // Library untuk NTP

// ==================== OBJEK LCD ====================
LiquidCrystal_I2C lcd(0x27, 16, 2); // Alamat 0x27, 16 kolom x 2 baris
#define SCL_PIN 9
#define SDA_PIN 8

// ==================== CONFIG NTP ====================
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 7 * 3600;  // WIB = GMT+7
const int   DAYLIGHT_OFFSET_SEC = 0;    // Indonesia tidak pakai DST

// Batasan jam kerja LDR (06:00 - 18:00)
const int LDR_START_HOUR = 6;   // Mulai jam 6 pagi
const int LDR_END_HOUR = 18;    // Sampai jam 6 sore

// ==================== CONFIG TREND BAROMETER ====================
const int   TREND_SLOTS        = 18;              // 3 jam @ setiap 10 menit
const unsigned long TREND_INTERVAL_MS = 600000UL; // 10 menit

float pressureHistory[TREND_SLOTS];
bool  pressureFilled   = false;
int   pressureIndex    = 0;
unsigned long lastTrendUpdate = 0;

// ==================== PIN SENSOR TAMBAHAN ====================
#define LDR_PIN   4    // ADC
#define RAIN_PIN  5    // ADC

// ==================== OBJEK GLOBAL ====================
Adafruit_BME280 bme;

// ==================== KONFIG WIFI & THINGSPEAK ====================
const char* WIFI_SSID     = "EVELIN";
const char* WIFI_PASSWORD = "12345678";

const char* THINGSPEAK_API_KEY = "3JBCYPZQB22NHOG4";
const char* THINGSPEAK_SERVER  = "api.thingspeak.com";

const unsigned long THINGSPEAK_INTERVAL_MS = 20000UL; // >15s agar aman dari rate limit
unsigned long lastThingSpeakSend = 0;

// ==================== ENUM & STRUCT PREDIKSI ====================
enum WeatherCode {
  CERAH = 0,
  CERAH_BERAWAN = 1,
  MENDUNG = 2,
  HUJAN_RINGAN = 3,
  HUJAN_DERAS = 4
};

struct WeatherPrediction {
  WeatherCode code;   // hasil prediksi cuaca
  int confidence;     // 0–100 %
  float deltaP;       // tren tekanan 3 jam (hPa)
};

// ==================== FUNGSI NTP ====================
void initNTP() {
  Serial.println("Menginisialisasi NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  // Tunggu sampai waktu tersinkronisasi
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    Serial.print(".");
    delay(1000);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nNTP berhasil tersinkronisasi!");
    Serial.print("Waktu sekarang: ");
    Serial.println(&timeinfo, "%A, %d %B %Y %H:%M:%S");
  } else {
    Serial.println("\nGagal mendapatkan waktu dari NTP");
  }
}

// Fungsi untuk mengecek apakah LDR boleh aktif (jam 06:00 - 18:00)
bool isLDRActiveTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // Jika gagal mendapatkan waktu, anggap LDR tidak aktif (aman)
    Serial.println("Gagal mendapatkan waktu lokal");
    return false;
  }
  
  int currentHour = timeinfo.tm_hour;
  
  // LDR aktif jika jam >= 6 pagi DAN jam < 6 sore
  return (currentHour >= LDR_START_HOUR && currentHour < LDR_END_HOUR);
}

// ==================== WIFI & THINGSPEAK HELPERS ====================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.print("Menghubungkan WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi terhubung, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Gagal terhubung ke WiFi");
  }
}

void ensureWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
}

bool sendDataToThingSpeak(float tempC, float hum, float pressHpa) {
  ensureWiFiConnected();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Kirim ThingSpeak gagal: WiFi tidak terhubung");
    return false;
  }

  WiFiClient client;
  if (!client.connect(THINGSPEAK_SERVER, 80)) {
    Serial.println("Kirim ThingSpeak gagal: koneksi server");
    return false;
  }

  String url = String("/update?api_key=") + THINGSPEAK_API_KEY +
               "&field1=" + String(tempC, 2) +
               "&field2=" + String(hum, 2) +
               "&field3=" + String(pressHpa, 2);

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + THINGSPEAK_SERVER + "\r\n" +
               "Connection: close\r\n\r\n");

  Serial.println("Kirim ThingSpeak: " + url);
  delay(300); // beri waktu transmisi singkat
  return true;
}

// ==================== FUNGSI TREND TEKANAN ====================
// Simpan tekanan ke circular buffer setiap 10 menit
void updatePressureHistory(float p_now_hpa) {
  pressureHistory[pressureIndex] = p_now_hpa;
  pressureIndex++;
  if (pressureIndex >= TREND_SLOTS) {
    pressureIndex = 0;
    pressureFilled = true;
  }
}

// Ambil delta tekanan 3 jam: P_now - P_3h_ago
bool getPressureTrend3h(float p_now_hpa, float &deltaP) {
  if (!pressureFilled) {
    return false; // belum cukup data 3 jam
  }
  int oldestIndex = (pressureIndex) % TREND_SLOTS; // slot berikutnya = data terlama
  float p_old = pressureHistory[oldestIndex];
  deltaP = p_now_hpa - p_old;
  return true;
}

// ==================== FUNGSI PREDIKSI CUACA TROPIS ====================
// pressure: hPa, humidity: %, temp: °C, deltaP: hPa
WeatherPrediction predictWeatherTropis(float pressure, float humidity, float temp, bool hasTrend, float deltaP, int ldrADC, int rainADC) {
  float scoreCerah        = 0;
  float scoreCerahBerawan = 0;
  float scoreMendung      = 0;
  float scoreHujanRingan  = 0;
  float scoreHujanDeras   = 0;

  // 1) Base dari tekanan udara
  if (pressure > 1015.0f) {
    scoreCerah        += 40;
    scoreCerahBerawan += 20;
    scoreMendung      += 0;
    scoreHujanRingan  += 0;
    scoreHujanDeras   += 0;
  }
  else if (pressure > 1010.0f && pressure <= 1015.0f) {
    scoreCerah        += 20;
    scoreCerahBerawan += 30;
    scoreMendung      += 10;
    scoreHujanRingan  += 0;
    scoreHujanDeras   += 0;
  }
  else if (pressure > 1005.0f && pressure <= 1010.0f) {
    scoreCerah        += 0;
    scoreCerahBerawan += 10;
    scoreMendung      += 30;
    scoreHujanRingan  += 15;
    scoreHujanDeras   += 0;
  }
  else { // pressure <= 1005
    scoreCerah        += 0;
    scoreCerahBerawan += 0;
    scoreMendung      += 15;
    scoreHujanRingan  += 30;
    scoreHujanDeras   += 40;
  }

  //2) Modifikasi dengan tren tekanan (kalau sudah valid)
  if (hasTrend) {
    if (deltaP > 2.0f) {
      // Tekanan naik -> cuaca membaik
      scoreCerah        += 20;
      scoreCerahBerawan += 10;
    }
    else if (deltaP >= -1.0f && deltaP <= 1.0f) {
    // Stabil -> tidak mengubah skor
    }
    else if (deltaP > -3.0f && deltaP < -1.0f) {
    // Turun perlahan -> awan & hujan ringan meningkat
    scoreMendung     += 15;
    scoreHujanRingan += 15;
    }
    else { // deltaP <= -3.0f
    // Turun cepat -> hujan kuat meningkat
    scoreMendung     += 10;
    scoreHujanRingan += 25;
    scoreHujanDeras  += 25;
    }
  }

  // 3) Modifikasi dengan kelembapan
  if (humidity < 60.0f) {
    // Udara kering -> awan tipis
    scoreCerah += 15;
  }
  else if (humidity > 80.0f) {
    // Udara lembap -> awan tebal & peluang hujan ringan naik
    scoreMendung     += 20;
    scoreHujanRingan += 15;
  }
  // 60–80% dianggap netral

  // 4) Konveksi tropis: siang panas + lembap + tekanan turun -> hujan lokal
  if (temp > 30.0f && humidity > 75.0f && hasTrend && deltaP < 0.0f) {
  scoreHujanRingan += 10;
  }
   
  // 5) Modifikasi dengan intensitas cahaya (LDR) - HANYA JIKA JAM AKTIF (06:00-18:00)
  // ADC ESP32: 0 (terang) - 4095 (gelap)
  bool ldrActive = isLDRActiveTime();
  
  if (ldrActive) {
    // LDR aktif pada jam siang (06:00 - 18:00)
    if (ldrADC > 3000) {
      // Sangat gelap -> awan tebal
      scoreMendung += 30;
    }
    else if (ldrADC > 1500 && ldrADC <= 3000) {
      // Terang sedang -> cerah berawan
      scoreCerahBerawan += 20;
    }
    else {
      // terang -> langit cerah
      scoreCerah += 30;
    }
    Serial.println("LDR aktif - Score ditambahkan");
  } else {
    // LDR tidak aktif di luar jam kerja (18:00 - 06:00)
    Serial.println("LDR non-aktif (di luar jam 06:00-18:00)");
  }
     
  // 6) Modifikasi dengan rain drop sensor
  // ADC ESP32: 0 (sangat basah) - 4095 (kering)
  if (rainADC < 1500) {
    // Basah banyak -> hujan deras
    scoreHujanDeras += 80;
  }
  else if (rainADC >= 1500 && rainADC < 3000) {
    // Gerimis / basah ringan
    scoreHujanRingan += 50;
  }
  // >=3000 dianggap kering -> tidak menambah skor

  // 7) Tentukan kategori berdasarkan skor tertinggi
  float scores[5] = {
    scoreCerah,
    scoreCerahBerawan,
    scoreMendung,
    scoreHujanRingan,
    scoreHujanDeras
  };

  int maxIndex = 0;
  float maxScore = scores[0];

  for (int i = 1; i < 5; i++) {
    if (scores[i] > maxScore) {
      maxScore = scores[i];
      maxIndex = i;
    }
  }

  // 8) Hitung confidence (kasar)
  float sumScore =
    scoreCerah +
    scoreCerahBerawan +
    scoreMendung +
    scoreHujanRingan +
    scoreHujanDeras;

  int confidence = 0;
  if (sumScore > 0.0f) {
    confidence = (int)((maxScore / sumScore) * 100.0f);
  }

  // 9) Susun dan kembalikan hasil prediksi
  WeatherPrediction wp;
  wp.code = (WeatherCode)maxIndex;   // mapping indeks skor ke enum cuaca
  wp.confidence = confidence;        // confidence hasil voting
  wp.deltaP = hasTrend ? deltaP : 0.0f;
  return wp;
}

  // ==================== HELPER: UBAH KODE CUACA JADI TEKS ====================
  String weatherCodeToText(WeatherCode code) {
    switch (code) {
      case CERAH:          return "Cerah";
      case CERAH_BERAWAN:  return "Cerah Berawan";
      case MENDUNG:        return "Mendung";
      case HUJAN_RINGAN:   return "Hujan Ringan";
      case HUJAN_DERAS:    return "Hujan Deras";
      default:             return "Unknown";
    }
  }

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(100);

  connectWiFi();

  // Inisialisasi NTP setelah WiFi terhubung
  if (WiFi.status() == WL_CONNECTED) {
    initNTP();
  }

  // I2C ESP32 (SDA = 8, SCL = 9)
  Wire.begin(SDA_PIN, SCL_PIN);

  // Init LCD 16x2 I2C
  lcd.init();
  lcd.backlight();
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Weather System");
  lcd.setCursor(0, 1);
  lcd.print("Initializing");

  // Init BME280 (alamat umum: 0x76 / 0x77)
  if (!bme.begin(0x76)) {
    if (!bme.begin(0x77)) {
      Serial.println("BME280 TIDAK TERDETEKSI!");

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("BME280 ERROR");
      lcd.setCursor(0, 1);
      lcd.print("Check wiring");

      while (1) {
        delay(10);
      }
    }
  }

  // Pin sensor tambahan (ADC)
  pinMode(LDR_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);

  // Inisialisasi history tekanan (trend 3 jam)
  for (int i = 0; i < TREND_SLOTS; i++) {
    pressureHistory[i] = 0.0f;
  }
  lastTrendUpdate = millis();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("System Ready");
  lcd.setCursor(0, 1);
  lcd.print("Monitoring...");

  Serial.println("=== Sistem Prediksi Cuaca ICAM ===");
  Serial.println("LCD 16x2 I2C aktif");
  Serial.println("Setup selesai.\n");
}

// ==================== LOOP ====================
void loop() {

  ensureWiFiConnected();

  // ===== BACA SENSOR =====
  float temp  = bme.readTemperature();          // °C
  float hum   = bme.readHumidity();             // %
  float press = bme.readPressure() / 100.0f;    // hPa

  int ldrADC  = analogRead(LDR_PIN);             // 0–4095
  int rainADC = analogRead(RAIN_PIN);            // 0–4095

    // ===== UPDATE TREND TEKANAN TIAP 10 MENIT =====
  unsigned long now = millis();
  if (now - lastTrendUpdate >= TREND_INTERVAL_MS) {
    updatePressureHistory(press);
    lastTrendUpdate = now;
  }

  float deltaP = 0.0f;
  bool hasTrend = getPressureTrend3h(press, deltaP);

  // ===== PREDIKSI CUACA =====
  WeatherPrediction pred = predictWeatherTropis(
    press,
    hum,
    temp,
    hasTrend,
    deltaP,
    ldrADC,
    rainADC
  );

  String predText = weatherCodeToText(pred.code);

    // ===== DEBUG SERIAL =====
  // Tampilkan waktu saat ini
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.print("Waktu  : ");
    Serial.println(&timeinfo, "%H:%M:%S");
  }
  
  Serial.println("====== Weather Data ======");
  Serial.print("Temp   : "); Serial.print(temp, 1); Serial.println(" C");
  Serial.print("Hum    : "); Serial.print(hum, 1);  Serial.println(" %");
  Serial.print("Press  : "); Serial.print(press, 1);Serial.println(" hPa");

  if (hasTrend) {
    Serial.print("dP3h   : "); 
    Serial.print(deltaP, 2); 
    Serial.println(" hPa");
  } else {
    Serial.println("dP3h   : data < 3 jam");
  }

  Serial.print("LDR ADC  : "); Serial.print(ldrADC);
  Serial.print(" (Status: ");
  Serial.print(isLDRActiveTime() ? "Aktif" : "Non-aktif");
  Serial.println(")");
  
  Serial.print("Rain ADC : "); Serial.println(rainADC);

  Serial.print("Prediksi : ");
  Serial.print(predText);
  Serial.print(" (");
  Serial.print(pred.confidence);
  Serial.println("%)");
  Serial.println();

  // ===== KIRIM KE THINGSPEAK (interval 20s) =====
  if (now - lastThingSpeakSend >= THINGSPEAK_INTERVAL_MS) {
    if (sendDataToThingSpeak(temp, hum, press)) {
      lastThingSpeakSend = now;
    }
  }

  // ===== TAMPILKAN DI LCD =====
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Cuaca:");
  lcd.setCursor(0, 1);
  lcd.print(predText);
  lcd.print(" ");
  lcd.print(pred.confidence);
  lcd.print("%");

  // ===== TUNGGU SEJENAK =====
  delay(2000); // tampilkan selama 2 detik sebelum pembacaan berikutnya
}