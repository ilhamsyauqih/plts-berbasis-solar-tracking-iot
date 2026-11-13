/*
  ==========================================================================
  SOLAR TRACKER IoT  -  ESP32 + 4 LDR + 2 Servo + LCD I2C 16x2 + Firebase
  Mata kuliah : Internet of Things
  --------------------------------------------------------------------------
  Library tambahan yang dibutuhkan (Library Manager):
    - ESP32Servo   (by Kevin Harrington / John K. Bennett)
  LCD I2C memakai driver sendiri di bawah (TIDAK perlu library LCD).
  Board: "ESP32 Dev Module"  (paket board: esp32 by Espressif Systems)
  Wi-Fi: hanya 2,4 GHz (ESP32 tidak mendukung 5 GHz)

  PIN
    LDR kiri-atas (TL)  = GPIO34      Servo PAN  (horizontal) = GPIO18
    LDR kanan-atas (TR) = GPIO35      Servo TILT (vertikal)   = GPIO19
    LDR kiri-bawah (BL) = GPIO32      LCD SDA = GPIO21
    LDR kanan-bawah (BR)= GPIO33      LCD SCL = GPIO22
    Semua LDR dapat 3V3 dari pin 3V3 ESP32 (tidak perlu kode)

  KONTROL DARI WEBSITE (Firebase)
    /solar/control = { "mode":"auto"|"manual", "az":0-180, "el":0-90 }
    Mode auto   : servo mengikuti cahaya (LDR)
    Mode manual : servo bergerak ke sudut az/el yang dikirim dari website
    Jika koneksi terputus > 20 detik saat manual, otomatis kembali ke auto.
  ==========================================================================
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ESP32Servo.h>

// ===================== KONFIGURASI (WAJIB DIISI) =====================
const char* WIFI_SSID = "Redmi_Note_10_Pro";
const char* WIFI_PASS = "12345678";
// URL Realtime Database dari Firebase Console (tanpa tanda "/" di akh ir)
const char* DB_URL    = "https://solar-tracker-e7605-default-rtdb.asia-southeast1.firebasedatabase.app";

// ===================== PIN =====================
const int PIN_LDR_TL   = 34;  // LDR kiri-atas
const int PIN_LDR_TR   = 35;  // LDR kanan-atas
const int PIN_LDR_BL   = 32;  // LDR kiri-bawah
const int PIN_LDR_BR   = 33;  // LDR kanan-bawah
const int PIN_SERVO_AZ = 18;  // servo 1 : PAN  (kiri-kanan)
const int PIN_SERVO_EL = 19;  // servo 2 : TILT (atas-bawah)
const int PIN_SDA      = 21;  // LCD I2C - SDA
const int PIN_SCL      = 22;  // LCD I2C - SCL

// ===================== PARAMETER (boleh diubah/kalibrasi) =====================
const int   DIR_AZ    = 1;        // ubah ke -1 jika arah PAN terbalik (mode AUTO)
const int   DIR_EL    = 1;        // ubah ke -1 jika arah TILT terbalik (mode AUTO)
const int   AZ_MIN = 0,  AZ_MAX = 180;
const int   EL_MIN = 0,  EL_MAX = 90;

// --- KEPEKAAN sensor (mode AUTO) ---
// Servo bereaksi pada selisih RELATIF antar-LDR (persen), jadi tetap peka walau cahaya redup / senter jauh.
const float TOL_REL  = 0.01f;     // selisih relatif yang dianggap seimbang (0,03 = 3 %). Kecil = lebih peka
const int   ABS_MIN  = 1;        // selisih ADC minimum agar noise tidak dianggap cahaya. Kecil = lebih peka
const float GAIN_REL = 55.0f;     // makin besar, servo makin cepat mengejar saat selisih membesar
const float MIN_STEP = 0.25f;     // langkah minimum per siklus saat selisih kecil (derajat)

// --- kecepatan & kehalusan gerak servo ---
// PRESET CEPAT (bawaan) : AUTO_MAX_STEP 3.0, EASE 0.40, MAX_SPEED 4.0, LDR_SMOOTH 0.6
// PRESET LEMBUT         : AUTO_MAX_STEP 1.0, EASE 0.20, MAX_SPEED 2.0, LDR_SMOOTH 0.25
const unsigned long LOOP_MS = 20;      // servo diperbarui tiap 20 ms (50 kali per detik)
const float AUTO_MAX_STEP = 3.0f;      // mode AUTO: langkah maksimum per siklus 20 ms (3,0 = 150 derajat/detik)
const float EASE          = 0.40f;     // 0-1: makin kecil = gerak makin lembut tetapi makin lambat
const float MAX_SPEED     = 4.0f;      // kecepatan maksimum servo (derajat per siklus 20 ms = 200 derajat/detik)
const float LDR_SMOOTH    = 0.6f;      // penghalus pembacaan LDR (0-1): makin kecil = makin stabil tetapi lambat bereaksi

// --- estimasi daya masuk dari panel (dihitung dari LDR, BUKAN hasil ukur) ---
const float PANEL_WP = 2.0f;           // daya puncak TOTAL 2 panel (Wp) -> isi sesuai label/datasheet panel

// --- jaringan (berjalan di task terpisah, tidak mengganggu servo) ---
const unsigned long SEND_MS  = 5000;   // interval kirim data ke Firebase
const unsigned long LCD_MS   = 1000;   // interval refresh LCD
const unsigned long PAGE_MS  = 3000;   // interval ganti halaman LCD
const unsigned long CTRL_TIMEOUT_MS = 20000;  // manual -> auto bila kontrol tak terbaca selama ini

// ===================== VARIABEL GLOBAL =====================
Servo servoAz, servoEl;
WiFiClientSecure secureClient;
HTTPClient http;                       // dipakai hanya oleh task jaringan (koneksi dipakai ulang)
int tl, tr, bl, br;                    // nilai ADC 4 LDR (sudah dihaluskan)
float tlf = 0, trf = 0, blf = 0, brf = 0;
int dV = 0, dH = 0;                    // selisih atas-bawah dan kiri-kanan
int lightPct = 0;                      // cahaya rata-rata (0-100 %)
float alignPct = 0;                    // keselarasan panel dengan cahaya (0-100 %)
float powerEst = 0;                    // estimasi daya masuk (W)
double energyWh = 0;                   // estimasi energi terkumpul sejak menyala (Wh)
float azPos = 90, elPos = 45;          // posisi servo saat ini (derajat, pecahan)
volatile float tgtAz = 90, tgtEl = 45; // sudut target (AUTO: dihitung LDR, MANUAL: dari website)
volatile bool manualMode = false;      // true = servo dikendalikan dari website
volatile int  lastHttp = 0;            // kode HTTP terakhir ke Firebase
const char* trackStatus = "Seimbang";
uint8_t lcdAddr = 0;                   // 0 = LCD tidak ditemukan
uint8_t lcdPage = 0;
unsigned long lastCtrlOk = 0;

// ============================================================
//  DRIVER LCD I2C 16x2 (modul PCF8574, mode 4-bit)
//  Pemetaan PCF8574: P0=RS P1=RW P2=EN P3=Backlight P4..P7=D4..D7
// ============================================================
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08

void pcfWrite(uint8_t v) {
  Wire.beginTransmission(lcdAddr);
  Wire.write(v | LCD_BL);
  Wire.endTransmission();
}
void lcdWrite4(uint8_t nib, uint8_t rs) {
  uint8_t v = (nib << 4) | rs;
  pcfWrite(v);
  pcfWrite(v | LCD_EN);
  delayMicroseconds(2);
  pcfWrite(v & ~LCD_EN);
  delayMicroseconds(60);
}
void lcdSend(uint8_t val, uint8_t rs) {
  lcdWrite4(val >> 4, rs);
  lcdWrite4(val & 0x0F, rs);
}
void lcdCmd(uint8_t c) { lcdSend(c, 0); if (c <= 2) delay(3); }

bool lcdDetect() {                      // cari alamat umum 0x27 dan 0x3F
  const uint8_t cand[2] = {0x27, 0x3F};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(cand[i]);
    if (Wire.endTransmission() == 0) { lcdAddr = cand[i]; return true; }
  }
  return false;
}
void lcdInit() {
  delay(50);
  lcdWrite4(0x03, 0); delay(5);
  lcdWrite4(0x03, 0); delayMicroseconds(150);
  lcdWrite4(0x03, 0);
  lcdWrite4(0x02, 0);                   // masuk mode 4-bit
  lcdCmd(0x28);                         // 2 baris, font 5x8
  lcdCmd(0x0C);                         // display ON, kursor OFF
  lcdCmd(0x06);                         // kursor bergeser ke kanan
  lcdCmd(0x01);                         // bersihkan layar
}
void lcdLine(uint8_t row, const char* s) {   // tulis tepat 16 karakter
  if (!lcdAddr) return;
  lcdCmd(row == 0 ? 0x80 : 0xC0);
  for (int i = 0; i < 16; i++) lcdSend(s[i] ? s[i] : ' ', LCD_RS);
}
void lcdShow2(const char* a, const char* b) {
  char l1[20], l2[20];
  snprintf(l1, sizeof l1, "%-16.16s", a);
  snprintf(l2, sizeof l2, "%-16.16s", b);
  lcdLine(0, l1); lcdLine(1, l2);
}

// ============================================================
//  SENSOR LDR (dengan penghalus)
// ============================================================
int readAvg(int pin) {                  // rata-rata 16 sampel, dalam milivolt lalu diskalakan ke 0..4095
  long sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogReadMilliVolts(pin); delayMicroseconds(60); }
  long v = (sum / 16) * 4095L / 3300L;
  return (int)constrain(v, 0L, 4095L);
}
void readSensors() {
  tlf += LDR_SMOOTH * (readAvg(PIN_LDR_TL) - tlf);
  trf += LDR_SMOOTH * (readAvg(PIN_LDR_TR) - trf);
  blf += LDR_SMOOTH * (readAvg(PIN_LDR_BL) - blf);
  brf += LDR_SMOOTH * (readAvg(PIN_LDR_BR) - brf);
  tl = (int)tlf; tr = (int)trf; bl = (int)blf; br = (int)brf;
  lightPct = ((tl + tr + bl + br) / 4) * 100 / 4095;
}

// ============================================================
//  GERAK SERVO: dihitung 50x per detik, mulus (easing)
// ============================================================
void servoWriteDeg(Servo& sv, float deg) {
  sv.writeMicroseconds(500 + (int)(deg * (1900.0f / 180.0f)));   // resolusi jauh lebih halus dari 1 derajat
}

void controlStep() {
  readSensors();
  int top = (tl + tr) / 2, bottom = (bl + br) / 2;
  int left = (tl + bl) / 2, right = (tr + br) / 2;
  dV = top - bottom;                    // positif = bagian atas lebih terang
  dH = left - right;                    // positif = bagian kiri lebih terang
  float errV = (float)dV / (float)(top + bottom + 40);    // selisih relatif atas-bawah (-1..+1)
  float errH = (float)dH / (float)(left + right + 40);    // selisih relatif kiri-kanan (-1..+1)

  // ESTIMASI daya masuk = daya puncak x cahaya x keselarasan (makin sejajar dengan cahaya, makin besar)
  float align = 1.0f - constrain(fabsf(errV) + fabsf(errH), 0.0f, 1.0f);
  alignPct = align * 100.0f;
  powerEst = PANEL_WP * (lightPct / 100.0f) * align;
  energyWh += (double)powerEst * LOOP_MS / 3600000.0;

  if (manualMode) {
    trackStatus = "Manual (web)";       // target diisi oleh task jaringan
  } else {
    bool moveEl = fabsf(errV) > TOL_REL && abs(dV) > ABS_MIN;
    bool moveAz = fabsf(errH) > TOL_REL && abs(dH) > ABS_MIN;
    float t;
    if (moveEl) {
      t = constrain((fabsf(errV) - TOL_REL) * GAIN_REL, MIN_STEP, AUTO_MAX_STEP);
      tgtEl += (dV > 0 ? t : -t) * DIR_EL;
    }
    if (moveAz) {
      t = constrain((fabsf(errH) - TOL_REL) * GAIN_REL, MIN_STEP, AUTO_MAX_STEP);
      tgtAz += (dH > 0 ? t : -t) * DIR_AZ;
    }
    if (moveAz && moveEl) trackStatus = "Koreksi PAN+TILT";
    else if (moveAz)      trackStatus = "Koreksi PAN";
    else if (moveEl)      trackStatus = "Koreksi TILT";
    else                  trackStatus = "Seimbang";
  }
  float ta = constrain((float)tgtAz, (float)AZ_MIN, (float)AZ_MAX);
  float te = constrain((float)tgtEl, (float)EL_MIN, (float)EL_MAX);
  tgtAz = ta; tgtEl = te;

  // posisi mendekati target secara bertahap (halus, tidak patah-patah)
  azPos += constrain((ta - azPos) * EASE, -MAX_SPEED, MAX_SPEED);
  elPos += constrain((te - elPos) * EASE, -MAX_SPEED, MAX_SPEED);
  servoWriteDeg(servoAz, azPos);
  servoWriteDeg(servoEl, elPos);
}

// ============================================================
//  LCD: 3 halaman bergantian
// ============================================================
void updateLcd() {
  char a[24], b[24];
  switch (lcdPage) {
    case 0:
      snprintf(a, sizeof a, "Pan:%3d Tilt:%3d", (int)(azPos + 0.5f), (int)(elPos + 0.5f));
      snprintf(b, sizeof b, "Cahaya:%3d%% %s", lightPct, manualMode ? "MANU" : "AUTO");
      break;
    case 1:
      snprintf(a, sizeof a, "TL:%4d TR:%4d", tl, tr);
      snprintf(b, sizeof b, "BL:%4d BR:%4d", bl, br);
      break;
    case 2:
      snprintf(a, sizeof a, "Daya est:%5.2f W", powerEst);
      snprintf(b, sizeof b, "Selaras:%3d%%", (int)(alignPct + 0.5f));
      break;
    default:
      if (WiFi.status() == WL_CONNECTED) snprintf(a, sizeof a, "WiFi:OK %4ddBm", WiFi.RSSI());
      else                               snprintf(a, sizeof a, "WiFi: terputus");
      if (lastHttp == 200) snprintf(b, sizeof b, "Firebase: OK");
      else                 snprintf(b, sizeof b, "Firebase: %d", (int)lastHttp);
      break;
  }
  lcdShow2(a, b);
}

// ============================================================
//  WI-FI & FIREBASE
// ============================================================
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Menyambung WiFi");
  lcdShow2("Menyambung WiFi", WIFI_SSID);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(500); Serial.print("."); }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " GAGAL (akan dicoba lagi)");
  lcdShow2(WiFi.status() == WL_CONNECTED ? "WiFi terhubung" : "WiFi gagal", "");
  delay(800);
}

// --- pembaca JSON sederhana (tanpa library) ---
bool jsonGetInt(const String& s, const char* key, int& out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k);
  if (i < 0) return false;
  out = s.substring(i + k.length()).toInt();
  return true;
}
bool jsonGetStr(const String& s, const char* key, String& out) {
  String k = String("\"") + key + "\":\"";
  int i = s.indexOf(k);
  if (i < 0) return false;
  i += k.length();
  int j = s.indexOf('"', i);
  if (j < 0) return false;
  out = s.substring(i, j);
  return true;
}

// Baca perintah kontrol dari website: /solar/control.json
void pollControl() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(DB_URL) + "/solar/control.json";
  http.begin(secureClient, url);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    lastCtrlOk = millis();
    bool wasManual = manualMode;
    String m; int v;
    bool nowManual = wasManual;
    if (jsonGetStr(body, "mode", m)) nowManual = (m == "manual");
    if (nowManual && !wasManual) { tgtAz = azPos; tgtEl = elPos; }        // mulai dari posisi sekarang
    if (nowManual) {
      if (jsonGetInt(body, "az", v)) tgtAz = constrain(v, AZ_MIN, AZ_MAX);
      if (jsonGetInt(body, "el", v)) tgtEl = constrain(v, EL_MIN, EL_MAX);
    }
    manualMode = nowManual;
  }
  http.end();
  // pengaman: koneksi kontrol putus saat manual -> kembali ke auto
  if (manualMode && millis() - lastCtrlOk > CTRL_TIMEOUT_MS) manualMode = false;
}

// Kirim data sensor & status ke website: /solar/live.json
void sendToFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  String json = "{";
  json += "\"ldr\":{\"tl\":" + String(tl) + ",\"tr\":" + String(tr) +
          ",\"bl\":" + String(bl) + ",\"br\":" + String(br) + "},";
  json += "\"servo\":{\"az\":" + String((int)(azPos + 0.5f)) + ",\"el\":" + String((int)(elPos + 0.5f)) + "},";
  json += "\"light\":" + String(lightPct) + ",";
  json += "\"align\":" + String((int)(alignPct + 0.5f)) + ",\"pest\":" + String(powerEst, 2) + ",";
  json += "\"wp\":" + String(PANEL_WP, 1) + ",\"ewh\":" + String((float)energyWh, 3) + ",";
  json += "\"dv\":" + String(dV) + ",\"dh\":" + String(dH) + ",";
  json += "\"mode\":\"" + String(manualMode ? "manual" : "auto") + "\",";
  json += "\"status\":\"" + String(trackStatus) + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"ts\":{\".sv\":\"timestamp\"}}";

  String url = String(DB_URL) + "/solar/live.json";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  lastHttp = http.PUT(json);            // 200 = berhasil
  Serial.printf("Firebase HTTP %d\n", (int)lastHttp);
  http.end();
}

// ============================================================
//  TASK JARINGAN (core 0): Firebase + LCD.
//  Dipisah dari loop() agar proses jaringan yang lambat
//  TIDAK membuat servo tersendat.
// ============================================================
void netTask(void* param) {
  unsigned long lastSend = 0, lastLcd = 0, lastPage = 0, lastWifiTry = 0;
  for (;;) {
    unsigned long now = millis();
    pollControl();                                   // baca perintah website
    if (now - lastSend >= SEND_MS) {
      lastSend = now;
      Serial.printf("TL=%d TR=%d BL=%d BR=%d | PAN=%.1f TILT=%.1f | Cahaya=%d%% | Daya est=%.2fW | %s | %s\n",
                    tl, tr, bl, br, azPos, elPos, lightPct, powerEst, manualMode ? "MANUAL" : "AUTO", trackStatus);
      sendToFirebase();
    }
    if (now - lastPage >= PAGE_MS) { lastPage = now; lcdPage = (lcdPage + 1) % 4; }
    if (now - lastLcd >= LCD_MS)   { lastLcd = now; updateLcd(); }
    if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 10000) {
      lastWifiTry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  SETUP & LOOP
// ============================================================
void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL);         // I2C bawaan ESP32: GPIO21 (SDA) dan GPIO22 (SCL)
  Wire.setClock(100000);
  if (lcdDetect()) {
    lcdInit();
    lcdShow2("Solar Tracker", "ESP32 + Firebase");
    Serial.printf("LCD ditemukan di 0x%02X\n", lcdAddr);
  } else {
    Serial.println("LCD I2C TIDAK ditemukan (cek kabel SDA/SCL/VCC/GND)");
  }

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoAz.setPeriodHertz(50);
  servoEl.setPeriodHertz(50);
  servoAz.attach(PIN_SERVO_AZ, 500, 2400);
  servoEl.attach(PIN_SERVO_EL, 500, 2400);
  servoWriteDeg(servoAz, azPos);
  servoWriteDeg(servoEl, elPos);

  // isi awal penghalus LDR agar tidak "melompat" saat pertama dinyalakan
  tlf = analogRead(PIN_LDR_TL); trf = analogRead(PIN_LDR_TR);
  blf = analogRead(PIN_LDR_BL); brf = analogRead(PIN_LDR_BR);

  secureClient.setInsecure();           // lewati verifikasi sertifikat (cukup untuk tugas kuliah)
  http.setReuse(true);                  // pakai ulang koneksi HTTPS agar kontrol terasa realtime
  http.setConnectTimeout(4000);
  http.setTimeout(3000);
  connectWifi();

  // jalankan task jaringan di core 0 (loop() berjalan di core 1)
  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);
}

void loop() {                           // hanya sensor + servo: cepat dan teratur
  static unsigned long last = 0;
  unsigned long now = millis();
  if (now - last >= LOOP_MS) {
    last += LOOP_MS;
    if (now - last > 5 * LOOP_MS) last = now;       // kejar ketertinggalan bila terlambat
    controlStep();
  }
}
