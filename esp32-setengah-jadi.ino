/***************** ESP32-S3 + RC522(VSPI) + SD(HSPI) + PCM5102A(I2S) + OLED 128x64 + USB QR *****************
 * Fitur:
 * - Suara      : bell pembuka → nama siswa → bell penutup, dari SD / server
 * - OLED       : tampilkan status, nama siswa (auto-scroll jika panjang)
 * - RFID + QR  : standby keduanya, RFID memakai find_id.php untuk dapatkan ID siswa
 * - SD & RC522 di SPI terpisah agar stabil (HSPI vs VSPI)
 * 
 * Library yang dibutuhkan:
 *  - ESP32-audioI2S by schreibfaul1
 *  - MFRC522 by miguelbalboa
 *  - Adafruit GFX + Adafruit SSD1306
 *  - (bawaan) WiFi, HTTPClient, SPI, SD, Wire
 ***********************************************************************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include <MFRC522.h>
#include "Audio.h"
#include "EspUsbHostKeybord.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

/* ================= WIFI ================= */
const char* ssid     = "Aha-Aha";
const char* password = "A1f4123@";

/* ================= SERVER ================= */
String serverVoice   = "http://scan.sditnurulrahmah.sch.id/index.php?id=";       // kirim mp3
String serverUpdate  = "http://scan.sditnurulrahmah.sch.id/update_uid.php?id=";  // simpan uid
String serverFindID  = "http://scan.sditnurulrahmah.sch.id/find_id.php?uid=";    // uid->id
String serverNameAPI = "http://scan.sditnurulrahmah.sch.id/get_name.php?id=";    // id->nama

/* ================ RC522 SPI (VSPI) ================= */
#define RC522_SCK  12
#define RC522_MISO 13
#define RC522_MOSI 11
#define RC522_SS   14
#define RC522_RST  16

/* ================ SD SPI (HSPI) ================= */
#define SD_SCK     10
#define SD_MISO    17
#define SD_MOSI    18
#define SD_CS       9
SPIClass hspi(HSPI);

/* ================ AUDIO I2S (PCM5102A) =============== */
#define I2S_BCLK    8
#define I2S_LRC     7
#define I2S_DOUT   46

/* ================ OLED I2C =========================== */
#define OLED_SDA   4
#define OLED_SCL   5
#define OLED_ADDR  0x3C
#define OLED_W     128
#define OLED_H      64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

/* ================ ADMIN =================== */
const char* ADMIN_ENTER = "ADMIN123";
const char* ADMIN_EXIT  = "ADMINEXIT";

/* ================ GLOBALS ================= */
Audio audio;
MFRC522 rfid(RC522_SS, RC522_RST);
unsigned long lastScanTime = 0;
const unsigned long SCAN_COOLDOWN = 1200; // 1.2 detik


class MyEspUsbHostKeybord : public EspUsbHostKeybord {
public:
  uint8_t prevKeys[6] = {0};
  String typedStr = "";
  bool* flag = nullptr;
  String* out = nullptr;

  char keycodeToChar(uint8_t keycode, bool shift) {
    if (keycode >= 4 && keycode <= 29) { char c = 'a' + (keycode - 4); return shift ? toupper(c) : c; }
    if (keycode >= 30 && keycode <= 38) { const char n[]="123456789"; const char s[]="!@#$%^&*("; return shift ? s[keycode-30] : n[keycode-30]; }
    if (keycode == 39) return shift ? ')' : '0';
    if (keycode == 40) return '\n';
    return 0;
  }

  void onKey(usb_transfer_t *t) override {
    uint8_t *p = t->data_buffer;
    bool shift = (p[0] & 0x22) != 0;
    uint8_t *keys = &p[2];
    for (int i = 0; i < 6; i++) {
      uint8_t k = keys[i]; if (!k) continue;
      bool seen=false; for(int j=0;j<6;j++) if(prevKeys[j]==k) seen=true;
      if(!seen){
        char ch=keycodeToChar(k,shift);
        if(ch=='\n'){ if(out && flag){ *out=typedStr; *flag=true; } typedStr=""; }
        else if(ch>0){ typedStr+=ch; }
      }
    }
    memcpy(prevKeys,keys,6);
  }
} usbHost;

String pendingID = ""; bool idReady = false;
bool adminMode = false; String adminTargetID = "";
bool busy = false; String currentID = "";

/* ==================== OLED HELPERS ===================== */
void oledPrintCenter(const String& line1, const String& line2="") {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(2);
  int16_t x1,y1; uint16_t w,h;
  oled.getTextBounds(line1, 0, 0, &x1,&y1, &w,&h);
  oled.setCursor((OLED_W - w)/2, 16);
  oled.print(line1);

  if(line2.length()){
    oled.getTextBounds(line2, 0, 0, &x1,&y1, &w,&h);
    oled.setCursor((OLED_W - w)/2, 38);
    oled.print(line2);
  }
  oled.display();
}

void oledTopLine(const String& s) {
  oled.fillRect(0,0,OLED_W,12,SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0,0);
  oled.print(s);
  oled.display();
}

void oledStatusReady()       { oledPrintCenter("Ready To", "Scan"); }
void oledStatusQR(const String& id)  { oledPrintCenter("QR:", id); }
void oledStatusRFID(const String& u) { oledPrintCenter("RFID:", u); }
void oledStatusAdmin()       { oledPrintCenter("ADMIN MODE", "Scan QR"); }
void oledStatusNoUID()       { oledPrintCenter("UID belum", "terdaftar"); }
void oledStatusDownloading() { oledPrintCenter("Downloading", "Audio..."); }

/* ========== Nama siswa dari server (id->nama) ========== */
String getStudentName(const String& id) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  String url = serverNameAPI + id;
  http.begin(url);
  int code = http.GET();
  String resp = "";
  if (code == 200) {
    resp = http.getString(); resp.trim();
    if (resp == "NOT_FOUND") resp = "";
  }
  http.end();
  return resp;
}

/* ==================== VOICE ===================== */
void ensureVoices() { if (!SD.exists("/voices")) SD.mkdir("/voices"); }

bool fetchVoice(String id) {
  String path = "/voices/" + id + ".mp3";
  // cache valid file
  if (SD.exists(path)) {
    File c = SD.open(path);
    if (c && c.size() > 20000) { c.close(); return true; }
    c.close();
  }
  WiFiClient client;
  HTTPClient http;
  String url = serverVoice + id;
  Serial.println("Downloading: " + url);
  oledStatusDownloading();

  for (int attempt = 1; attempt <= 3; attempt++) {
    if (!http.begin(client, url)) return false;
    http.setTimeout(15000);
    int code = http.GET();
    if (code != 200) { http.end(); delay(700); continue; }

    int totalLength = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    File f = SD.open(path, FILE_WRITE);
    if (!f) { http.end(); return false; }

    uint8_t buff[512];
    int len = totalLength;
    size_t totalWritten = 0;
    unsigned long lastRead = millis();

    while (http.connected() && (len > 0 || totalLength == -1)) {
      int available = stream->available();
      if (available) {
        int readBytes = stream->readBytes(buff, min(available, (int)sizeof(buff)));
        f.write(buff, readBytes);
        totalWritten += readBytes;
        len -= readBytes;
        lastRead = millis();
      } else {
        if (millis() - lastRead > 4000) break;
        delay(10);
      }
    }
    f.close();
    http.end();
    if (totalWritten > 20000) return true; // valid
    SD.remove(path);
  }
  return false;
}

/* ===== OLED scrolling selama playback ===== */
void drawScrollingNameFrame(const String& title, const String& name, int scrollX) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  // title kecil di atas
  oled.setTextSize(1);
  oled.setCursor(0,0);
  oled.print(title);
  // nama besar scroll
  oled.setTextSize(2);
  oled.setCursor(scrollX, 24);
  oled.print(name);
  oled.display();
}

/* ======= Main player: bell open -> nama -> bell close ======= */
void playVoice(String id) {
  String voicePath  = "/voices/" + id + ".mp3";
  String bellOpen   = "/voices/bell.mp3";
  String bellClose  = "/voices/belled.mp3";

  //  Ambil nama siswa
  String studentName = getStudentName(id);
  if (studentName == "") studentName = id;

  //  Hitung lebar teks
  oled.setTextSize(2);
  int16_t x1,y1; 
  uint16_t wText,hText;
  oled.getTextBounds(studentName, 0, 0, &x1,&y1, &wText,&hText);

  bool needScroll = (studentName.length() > 10);   // Scroll kalau nama panjang
  int scrollX = OLED_W; // mulai dari kanan

  audio.stopSong();
  delay(50);
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(8);
  audio.setInBufferSize(64 * 1024);

  //  Pastikan file sudah ada
  if (!SD.exists(voicePath)) {
    if (!fetchVoice(id)) return;
  }
  //  Main voice
  audio.connecttoFS(SD, voicePath.c_str());

  unsigned long lastFrame = 0;

  while (audio.isRunning()) {
    audio.loop();

    unsigned long now = millis();
    if (now - lastFrame > 33) {  // redraw tiap 33ms (~30FPS)
      lastFrame = now;

      oled.clearDisplay();
      oled.setTextSize(1);
      oled.setCursor(0,0);
      oled.print("Memanggil:");

      oled.setTextSize(2);
      if (!needScroll) {
        // nama pendek → tampilkan langsung
        oled.setCursor(0, 24);
        oled.print(studentName);
      } else {
        //  SCROLL KIRI
        oled.setCursor(scrollX, 24);
        oled.print(studentName);

        scrollX -= 2; // ke kiri
        if (scrollX < -((int)wText)) {
          scrollX = OLED_W; // ulang lagi
        }
      }
      oled.display();
    }
    delay(1);
  }
  oledStatusReady();
}


/* ===================== DB UID SEND =================== */
bool sendUIDtoServer(String id, String uid){
  HTTPClient http;
  String url = serverUpdate + id + "&uid=" + uid;
  http.begin(url);
  int code = http.GET();
  http.end();
  return code == 200;
}

/* ===================== LOOKUP ID BY UID =================== */
String lookupIdByUid(const String &uid) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient http;
  String url = serverFindID + uid;
  http.begin(url);
  int code = http.GET();
  String resp = "";
  if (code == 200) {
    resp = http.getString(); resp.trim();
    if (resp == "" || resp == "NOT_FOUND") resp = "";
  }
  http.end();
  return resp;
}

/* ===================== ABSEN ======================== */
void processID(String id){
  if (WiFi.status() == WL_CONNECTED) {
    if (fetchVoice(id)) { playVoice(id); return; }
  }
  if (SD.exists("/voices/"+id+".mp3")) playVoice(id);
}

/* ===================== SETUP ======================== */

void oledBoot(){
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init FAIL");
    return;
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0,0);
  oled.println("Booting...");
  oled.display();
}

void setup(){
  Serial.begin(115200);
  delay(400);
  oledBoot();

  pinMode(SD_CS, OUTPUT);
  pinMode(RC522_SS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  digitalWrite(RC522_SS, HIGH);

  // RC522 on VSPI
  SPI.begin(RC522_SCK, RC522_MISO, RC522_MOSI);
  rfid.PCD_Init();
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  Serial.printf("RC522 VersionReg: 0x%02X\n", v);
  oledTopLine(String("RC522: ") + ((v==0x91||v==0x92)?"OK":"FAIL"));

  // SD on HSPI
  hspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, hspi, 4000000)) {
    Serial.println("SD OK");
    oledTopLine("SD: OK");
    ensureVoices();
  } else {
    Serial.println("SD FAIL");
    oledTopLine("SD: FAIL");
  }

  // Audio (PCM5102A)
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(8);
  audio.setInBufferSize(64*1024);

  // WiFi
  WiFi.begin(ssid,password);
  uint32_t t=millis(); Serial.print("WiFi");
  while(WiFi.status()!=WL_CONNECTED && millis()-t<7000){ Serial.print("."); delay(250); }
  Serial.println(WiFi.status()==WL_CONNECTED?" OK":" FAIL");
  oledTopLine(String("WiFi: ") + (WiFi.status()==WL_CONNECTED?"OK":"FAIL"));

  // USB Host
  usbHost.begin();
  usbHost.out  = &pendingID;
  usbHost.flag = &idReady;

  oledStatusReady();
  Serial.println(" Ready for scan (QR / RFID)");
}

/* ===================== LOOP ========================= */
void loop(){
  usbHost.task();
  audio.loop();

  if (busy) {
    if (!audio.isRunning()) { busy = false; oledStatusReady(); Serial.println(" Ready"); }
    return;
  }

  // --- ADMIN MODE ---
  if (adminMode) {
    if (adminTargetID == "") {
      if (idReady && pendingID.length()>0) {
        adminTargetID = pendingID;
        idReady=false; pendingID="";
        Serial.println(" QR ID siswa: " + adminTargetID);
        oledPrintCenter("ADMIN:", adminTargetID);
        Serial.println("Tempel kartu...");
      }
      return;
    }

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String uid="";
      for (byte i=0;i<rfid.uid.size;i++){ char t[3]; sprintf(t,"%02X",rfid.uid.uidByte[i]); uid+=t; }
      rfid.PICC_HaltA();
      Serial.println("UID: " + uid);
      oledPrintCenter("Write UID", uid);

      if (sendUIDtoServer(adminTargetID, uid))
        Serial.println(" UID saved");
      else
        Serial.println(" UID save FAIL");

      adminMode=false; adminTargetID="";
      oledStatusReady();
      return;
    }
    return;
  }

  // --- RFID ---
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid="";
    for (byte i=0;i<rfid.uid.size;i++){ char t[3]; sprintf(t,"%02X",rfid.uid.uidByte[i]); uid+=t; }
    rfid.PICC_HaltA();

    if (uid == ADMIN_ENTER) { adminMode=true; oledStatusAdmin(); Serial.println(" ADMIN MODE"); return; }

    Serial.println("RFID: " + uid);
    oledStatusRFID(uid);

    String idFromServer = lookupIdByUid(uid);
    if (idFromServer == "") {
      Serial.println(" UID belum terdaftar!");
      oledStatusNoUID();
      if (SD.exists("/voices/not_registered.mp3")) {
        audio.stopSong();
        audio.connecttoFS(SD, "/voices/not_registered.mp3");
        while (audio.isRunning()) { audio.loop(); delay(3); }
      }
      oledStatusReady();
      return;
    }

    busy=true;
    currentID=idFromServer;
    processID(currentID);
    return;
  }

  // --- QR ---
  if (idReady && pendingID.length()>0) {
    // mencegah scan ganda dalam waktu cepat
    if (millis() - lastScanTime < SCAN_COOLDOWN) {
        Serial.println(" QR diabaikan (double scan)");
        idReady = false;
        pendingID = "";
        return;
    }
    lastScanTime = millis();
    String id = pendingID;
    idReady=false; pendingID="";

    if (id == ADMIN_ENTER) { adminMode=true; oledStatusAdmin(); Serial.println(" ADMIN MODE"); return; }

    Serial.println("QR: " + id);
    oledStatusQR(id);

    busy=true;
    currentID=id;
    processID(id);
  }
}
