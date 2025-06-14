
// Library yang digunakan
#include <WiFi.h>
#include <FirebaseESP32.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Servo.h>
#include <RTClib.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <vector>
#include <set>
#include <algorithm>

// Konfigurasi WiFi
#define WIFI_SSID       "Esp"
#define WIFI_PASSWORD   "esp12345678"

// Konfigurasi Firebase
#define API_KEY         "AIzaSyAT-heeFEQAmvCi7mXO9nLwv_7WNh-qHNI"
#define DATABASE_URL    "https://sipakis-dashboard-default-rtdb.firebaseio.com"
#define USER_EMAIL      "sipakis@gmail.com"
#define USER_PASSWORD   "sipakis123"

// Pin yang digunakan
#define TRIG_PIN 12
#define ECHO_PIN 14
#define SERVO_PIN 13

// Inisialisasi LCD, servo, dan objek Firebase
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servo;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// NTP Client untuk waktu internet
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200); // UTC+7

// RTC module (DS3231)
RTC_DS3231 rtc;

// Menyimpan jadwal aktif dan jadwal yang sudah dipakai hari ini
std::vector<String> jadwalAktif;
std::set<String> jadwalSudahPakan;
String tanggalHariIni = "";

// Timer
unsigned long waktuTerakhirPakan = 0;
const unsigned long jedaPakanMillis = 60000; // Delay 1 menit antar pakan

// Menyimpan status persentase terakhir
int lastPersen = -1;

// Untuk fitur slide LCD
unsigned long waktuTerakhirSlide = 0;
bool tampilanPakanSekarang = true;

// Fungsi konversi waktu "HH:MM" ke menit
int waktuKeMenit(const String& waktu) {
  if (waktu.length() < 5) return -1;
  int jam = waktu.substring(0, 2).toInt();
  int menit = waktu.substring(3, 5).toInt();
  return jam * 60 + menit;
}

// Fungsi membaca jarak dari sensor ultrasonik
long bacaJarak() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long durasi = pulseIn(ECHO_PIN, HIGH, 30000); // batas waktu 30ms
  if (durasi == 0) return 999; // error
  return durasi * 0.034 / 2; // konversi ke cm
  
}

// Konversi jarak ke persentase isi pakan
int jarakKePersen(long jarak) {
  int persen = map(jarak, 15, 4, 0, 100); // 22 cm = 0%, 4 cm = 100%
  return constrain(persen, 0, 100);
}


// Fungsi memberi pakan
void beriPakan() {
  Serial.println("Servo memberi pakan...");
  servo.write(90); // buka pakan
  delay(1000);
  servo.write(0);  // tutup pakan

  // Ambil waktu sekarang
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);

  // Format tanggal dan jam
  char tanggal[11], jam[6];
  strftime(tanggal, sizeof(tanggal), "%d/%m/%Y", t);
  strftime(jam, sizeof(jam), "%H.%M", t);

  // Simpan ke Firebase
  FirebaseJson json;
  json.set("tanggal", tanggal);
  json.set("jam", jam);
  json.set("status", "Sukses");

  if (Firebase.pushJSON(fbdo, "/riwayat", json)) {
    Serial.println("Berhasil catat riwayat pakan.");
  } else {
    Serial.print("Gagal catat riwayat: ");
    Serial.println(fbdo.errorReason());
  }
}

// Ambil dan perbarui jadwal aktif dari Firebase
void updateJadwalAktif() {
  jadwalAktif.clear();
  if (Firebase.getJSON(fbdo, "/jadwal")) {
    FirebaseJson &json = fbdo.jsonObject();
    size_t len = json.iteratorBegin();

    for (size_t i = 0; i < len; i++) {
      String key, value;
      int type;
      json.iteratorGet(i, type, key, value);

      FirebaseJson obj(value);
      FirebaseJsonData waktuData, statusData;
      obj.get(waktuData, "waktu");
      obj.get(statusData, "status");

      if (statusData.to<String>() == "Aktif") {
        jadwalAktif.push_back(waktuData.to<String>());
      }
    }
    json.iteratorEnd();
    std::sort(jadwalAktif.begin(), jadwalAktif.end()); // urutkan waktu
  } else {
    Serial.println("Gagal mendapatkan jadwal dari Firebase");
  }
}

// Menampilkan status pakan dan jadwal di LCD secara bergantian
void tampilkanLCDSlide(const String& waktuSekarang, const String& statusPakan, const String& pakanSelanjutnya) {
  if (millis() - waktuTerakhirSlide >= 3000) {
    tampilanPakanSekarang = !tampilanPakanSekarang;
    waktuTerakhirSlide = millis();
    lcd.clear();
  }

  if (tampilanPakanSekarang) {
    lcd.setCursor(0, 0);
    lcd.print("Pakan : " + statusPakan);
    lcd.setCursor(0, 1);
    lcd.print("Jam   : " + waktuSekarang);
  } else {
    lcd.setCursor(0, 0);
    lcd.print("Pakan Selanjutnya:");
    lcd.setCursor(0, 1);
    lcd.print(pakanSelanjutnya);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Wire.begin(21, 22); // SDA = 21, SCL = 22
  lcd.init();
  lcd.backlight();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sipakis Siap");

  // Hubungkan ke WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Terhubung!");

  // Sinkronisasi waktu dari internet
  configTime(25200, 0, "pool.ntp.org");
  while (time(nullptr) < 100000) {
    Serial.println("Sinkronisasi waktu...");
    delay(500);
  }

  // Konfigurasi Firebase
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  servo.attach(SERVO_PIN);
  servo.write(0); // posisi awal servo

  rtc.begin();
  timeClient.begin(); // mulai NTP

  Firebase.setInt(fbdo, "/jumlah_pakan", 100); // inisialisasi
}

void loop() {
  timeClient.update(); // update waktu

  String waktuSekarang = timeClient.getFormattedTime().substring(0, 5);

  // Ambil tanggal hari ini
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char bufferTanggal[11];
  strftime(bufferTanggal, sizeof(bufferTanggal), "%d/%m/%Y", t);
  String tanggalSekarang(bufferTanggal);

  // Jika berganti hari, reset set jadwal sudah dipakai
  if (tanggalSekarang != tanggalHariIni) {
    tanggalHariIni = tanggalSekarang;
    jadwalSudahPakan.clear();
    Serial.println("Reset jadwal sudah pakan hari ini.");
  }

  // Baca jarak dan konversi ke persentase
long jarak = bacaJarak();
Serial.print("Jarak Ultrasonik: ");
Serial.print(jarak);
Serial.println(" cm");

int persen = jarakKePersen(jarak);
  Firebase.setInt(fbdo, "/jumlah_pakan", persen);

  // Update jadwal aktif dari Firebase
  updateJadwalAktif();

  // Cek apakah waktu saat ini cocok untuk jadwal pakan
  int waktuMenitSekarang = waktuKeMenit(waktuSekarang);
  bool beriPakanSekarang = false;
  String jadwalPakanSaatIni = "";

  for (auto& jadwal : jadwalAktif) {
    if (jadwal == waktuSekarang && jadwalSudahPakan.find(jadwal) == jadwalSudahPakan.end()) {
      if (millis() - waktuTerakhirPakan > jedaPakanMillis) {
        beriPakanSekarang = true;
        jadwalPakanSaatIni = jadwal;
        break;
      }
    }
  }

  // Jika waktunya pakan
  if (beriPakanSekarang) {
    beriPakan();
    waktuTerakhirPakan = millis();
    jadwalSudahPakan.insert(jadwalPakanSaatIni);
  }

  // Jika tombol "Pakan Sekarang" ditekan di Firebase
  if (Firebase.getBool(fbdo, "/perintah/pakan_sekarang") && fbdo.boolData()) {
    beriPakan();
    Firebase.setBool(fbdo, "/perintah/pakan_sekarang", false);
  }

  // Tentukan status isi pakan
  String statusPakan;
  if (persen <= 0) {
    statusPakan = "Habis";
  } else if (persen <= 40) {
    statusPakan = "Sedikit";
  } else {
    statusPakan = "Banyak";
  }

  // Cari jadwal pakan selanjutnya
  String pakanSelanjutnya = "-";
  for (auto& j : jadwalAktif) {
    if (jadwalSudahPakan.find(j) == jadwalSudahPakan.end() && waktuKeMenit(j) > waktuMenitSekarang) {
      pakanSelanjutnya = j;
      break;
    }
  }
  if (pakanSelanjutnya == "-" && !jadwalAktif.empty()) {
    pakanSelanjutnya = jadwalAktif[0]; // fallback ke jadwal pertama
  }

  // Tampilkan di LCD
  tampilkanLCDSlide(waktuSekarang, statusPakan, pakanSelanjutnya);

  delay(500); // delay utama loop
}