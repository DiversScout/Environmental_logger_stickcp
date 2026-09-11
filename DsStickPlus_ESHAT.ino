/*
  M5Unified ENV-III HAT (M5StickCPlus向け)
  + Config Mode (WiFi AP & WebServer) + Leaflet Map
  + Supabase REST API Data Logging
  + WBGT Calculation & Split Display Layout
*/

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <cmath>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WebServer.h>

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedENV.h>

// ===== Wi-Fi / Supabase パラメータ =====
static const char* WSSID     = "Please change it";
static const char* WPASSWORD = "Please change it";

// Supabase URL (例: https://xxxx.supabase.co) ※末尾の / は不要
static const char* SUPABASE_URL  = "https://_supabase_projectid_.supabase.co";
static const char* SUPABASE_KEY  = "sb_secret_sUpAbAseSecrEtKey_ReWR1t3H3r3";

static constexpr uint32_t SUPABASE_INTERVAL_MS = 60000; // 送信間隔（60秒）
static uint32_t gLastSupabaseSendMs = 0;
static int gSupabaseHttpCode = 0; // 追加：Supabaseへの通信状態を保持

// ===== 設定情報 (Preferences保存用) =====
String gSSID = "ACCESSPOINT_SSID";
String gPassword = "ACCESSPOINT_SECRET";
float gLat = 33.593114543982345;
float gLon = 130.40195761638657;
String gDeviceId = "_DEVICE_ID_";
String gDeviceName = "_DEVICE_NAME_";

Preferences prefs;
WebServer server(80);
bool gConfigMode = false;

// =====================================
// 画面レイアウトパラメータ (横向き 240x135)
static const int LCD_W = 240, LCD_H = 135;
static const int ENV_W = 160; // 左側2/3
static const int IMU_X = 160; // 右側1/3の開始X座標

// ── ENV-III（HAT ENV-III）─────────────────────────────
m5::unit::UnitUnified Units;
m5::unit::UnitENV3 unitENV3;
auto& sht30   = unitENV3.sht30;
auto& qmp6988 = unitENV3.qmp6988;

static bool  gEnvReady = false;
static float gEnv_sht30_tempC = NAN;
static float gEnv_sht30_humRH = NAN;
static float gEnv_qmp6988_pressPa = NAN;

M5Canvas canvas(&M5.Display);

// ================= Configモード(Webサーバー) 処理 =================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  // Leaflet.js の CSS と JS を読み込み (インターネット接続が必要)
  html += "<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'/>";
  html += "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>";
  
  html += "<style>";
  html += "body { font-family: sans-serif; padding: 10px; }";
  html += "input[type='text'], input[type='password'] { width: 100%; padding: 8px; margin-bottom: 10px; box-sizing: border-box; }";
  html += "#map { height: 300px; width: 100%; margin-bottom: 15px; border: 1px solid #ccc; }";
  html += ".btn { padding: 12px; background-color: #007BFF; color: white; border: none; border-radius: 5px; width: 100%; font-size: 16px; cursor: pointer; }";
  html += "</style>";
  html += "</head><body>";
  
  html += "<h2>M5StickCPlus Config</h2>";
  html += "<form action='/save' method='POST'>";
  
  html += "<label>Device ID:</label><br>";
  html += "<input type='text' name='devid' value='" + gDeviceId + "'><br>";
  html += "<label>Device Name:</label><br>";
  html += "<input type='text' name='devname' value='" + gDeviceName + "'><br>";
  
  html += "<label>SSID:</label><br>";
  html += "<input type='text' name='ssid' value='" + gSSID + "'><br>";
  html += "<label>Password:</label><br>";
  html += "<input type='password' name='pass' value='" + gPassword + "'><br>";
  
  html += "<hr>";
  html += "<h3>Location Settings</h3>";
  html += "<p style='font-size: 14px; color: #555;'>ブラウザの現在地取得を許可するか、地図をタップして位置を指定してください。</p>";
  
  // 地図表示用コンテナ
  html += "<div id='map'></div>";
  
  html += "<label>Latitude (緯度):</label><br>";
  html += "<input type='text' id='latInput' name='lat' value='" + String(gLat, 6) + "' ><br>";
  html += "<label>Longitude (経度):</label><br>";
  html += "<input type='text' id='lonInput' name='lon' value='" + String(gLon, 6) + "' ><br><br>";
  
  html += "<input type='submit' class='btn' value='Save & Restart'>";
  html += "</form>";

  // 地図制御用 JavaScript
  html += "<script>";
  html += "var initialLat = " + String(gLat, 6) + ";";
  html += "var initialLon = " + String(gLon, 6) + ";";
  html += "var map = L.map('map').setView([initialLat, initialLon], 13);";
  
  html += "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {";
  html += "  attribution: '&copy; OpenStreetMap contributors'";
  html += "}).addTo(map);";
  
  html += "var marker = L.marker([initialLat, initialLon]).addTo(map);";
  
  // 地図クリック時の処理
  html += "map.on('click', function(e) {";
  html += "  var lat = e.latlng.lat;";
  html += "  var lon = e.latlng.lng;";
  html += "  marker.setLatLng([lat, lon]);";
  html += "  document.getElementById('latInput').value = lat.toFixed(6);";
  html += "  document.getElementById('lonInput').value = lon.toFixed(6);";
  html += "});";

  // スマホのGPS（Geolocation API）による現在地取得 (HTTPS環境でのみ動作)
  html += "if (navigator.geolocation) {";
  html += "  navigator.geolocation.getCurrentPosition(function(position) {";
  html += "    var lat = position.coords.latitude;";
  html += "    var lon = position.coords.longitude;";
  html += "    map.setView([lat, lon], 15);";
  html += "    marker.setLatLng([lat, lon]);";
  html += "    document.getElementById('latInput').value = lat.toFixed(6);";
  html += "    document.getElementById('lonInput').value = lon.toFixed(6);";
  html += "  }, function(error) {";
  html += "    console.log('Geolocation error: ' + error.message);";
  html += "  });";
  html += "}";
  html += "</script>";

  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("devid")) {
    gDeviceId = server.arg("devid");
    prefs.putString("devid", gDeviceId);
  }
  if (server.hasArg("devname")) {
    gDeviceName = server.arg("devname");
    prefs.putString("devname", gDeviceName);
  }
  if (server.hasArg("ssid")) {
    gSSID = server.arg("ssid");
    prefs.putString("ssid", gSSID);
  }
  if (server.hasArg("pass")) {
    gPassword = server.arg("pass");
    prefs.putString("pass", gPassword);
  }
  if (server.hasArg("lat")) {
    gLat = server.arg("lat").toFloat();
    prefs.putFloat("lat", gLat);
  }
  if (server.hasArg("lon")) {
    gLon = server.arg("lon").toFloat();
    prefs.putFloat("lon", gLon);
  }
  
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body style='font-family:sans-serif; text-align:center; padding: 20px;'>";
  html += "<h2>Saved! Restarting...</h2><p>設定を保存しました。<br>端末が再起動しますので、ブラウザを閉じてください。</p></body></html>";
  server.send(200, "text/html", html);
  
  delay(1000);
  ESP.restart(); // 再起動して設定を反映
}

void enterConfigMode() {
  gConfigMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("M5Stick_Config"); // スマホから見えるアクセスポイント名
  
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();
  
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(10, 10);
  M5.Display.println("Config Mode");
  
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 45);
  M5.Display.println("1. Connect WiFi to:");
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.setCursor(15, 60);
  M5.Display.println("M5Stick_Config");
  
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setCursor(10, 85);
  M5.Display.println("2. Open browser:");
  M5.Display.setTextColor(TFT_CYAN);
  M5.Display.setCursor(15, 100);
  M5.Display.println("http://192.168.4.1");
}

// ================= WBGT 計算処理 =================
float calculateWBGT(float temp, float hum) {
  if (isnan(temp) || isnan(hum)) return NAN;
  return 0.735 * temp + 0.0374 * hum + 0.00292 * temp * hum + 7.619;
}

uint16_t getWbgtColor(float wbgt) {
  if (isnan(wbgt)) return TFT_WHITE;
  if (wbgt >= 31.0) return canvas.color565(255, 0, 0);     // (赤) 危険
  if (wbgt >= 28.0) return canvas.color565(255, 153, 0);   // (橙) 厳重警戒
  if (wbgt >= 25.0) return canvas.color565(255, 255, 0);   // (黄) 警戒
  if (wbgt >= 21.0) return canvas.color565(153, 204, 255); // (水色) 注意
  return canvas.color565(51, 153, 255);                    // (青) ほぼ安全
}
/*
uint16_t getWbgtColor(float wbgt) {
  if (isnan(wbgt)) return TFT_WHITE;
  if (wbgt >= 31.0) return TFT_RED;
  if (wbgt >= 28.0) return TFT_ORANGE;
  if (wbgt >= 25.0) return TFT_YELLOW;
  if (wbgt >= 21.0) return TFT_CYAN;
  return canvas.color565(100, 150, 255);
}
*/

// ================= Wi-Fi 接続維持処理 =================
static uint32_t gLastWiFiCheckMs = 0;
static void checkWiFiConnection(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (now - gLastWiFiCheckMs >= 10000) {
      Serial.println("[WiFi] Connection lost. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(gSSID.c_str(), gPassword.c_str());
      gLastWiFiCheckMs = now;
    }
  }
}

// ================= Supabase への送信処理 =================
static void sendToSupabase(float temp, float wbgt) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Supabase] WiFi not connected. Skip sending.");
    return;
  }
  if (isnan(temp)) return;

  WiFiClientSecure client;
  client.setInsecure(); // ルート証明書検証を省略

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_current_status?on_conflict=device_id";
  http.begin(client, url);

  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "resolution=merge-duplicates");

  struct tm ti;
  char timeBuf[32] = "1970-01-01T00:00:00+09:00";
  if (getLocalTime(&ti)) {
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S+09:00", &ti);
  }

  const char* statusStr = "safe";
  if (wbgt >= 31.0) statusStr = "danger";
  else if (wbgt >= 28.0) statusStr = "severe_warning";
  else if (wbgt >= 25.0) statusStr = "warning";
  else if (wbgt >= 21.0) statusStr = "attention";

  char payload[768]; 
  snprintf(payload, sizeof(payload),
    "{\"device_id\":\"%s\","
    "\"device_name\":\"%s\","
    "\"is_fixed\":true,"
    "\"jst\":\"%s\","
    "\"temperature\":%.2f,"
    "\"status\":\"%s\","
    "\"lat\":%.6f,"
    "\"lon\":%.6f}",
    gDeviceId.c_str(), gDeviceName.c_str(), timeBuf, temp, statusStr, gLat, gLon);

  int httpCode = http.POST(payload);
  
  gSupabaseHttpCode = httpCode; // 追加：結果をグローバル変数に保存

  if (httpCode > 0) {
    Serial.printf("[Supabase] POST success: %d\n", httpCode);
  } else {
    Serial.printf("[Supabase] POST failed: %d %s\n", httpCode, http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ================= 画面描画処理 =================
static void drawScreen() {
  canvas.fillSprite(TFT_BLACK);

  // --- 左側 2/3 (ENV3 & WBGT) ---
  float wbgt = calculateWBGT(gEnv_sht30_tempC, gEnv_sht30_humRH);

  canvas.setTextSize(2);
  canvas.setTextColor(TFT_WHITE);
  
  canvas.setCursor(10, 15);
  if (!isnan(gEnv_sht30_tempC)) {
    canvas.printf("Temp: %.1f C", gEnv_sht30_tempC);
  } else {
    canvas.print("Temp: --- C");
  }

  canvas.setCursor(10, 45);
  if (!isnan(gEnv_sht30_humRH)) {
    canvas.printf("Hum : %.1f %%", gEnv_sht30_humRH);
  } else {
    canvas.print("Hum : --- %");
  }

  canvas.setTextSize(2);
  canvas.setTextColor(getWbgtColor(wbgt));
  canvas.setCursor(10, 75);
  if (!isnan(wbgt)) {
    canvas.printf("WBGT: %.1f", wbgt);
  } else {
    canvas.print("WBGT: ---");
  }

  // --- 通信ステータス表示 ---
  canvas.setTextSize(1);
  if (WiFi.status() == WL_CONNECTED) {
    if (gSupabaseHttpCode == 200 || gSupabaseHttpCode == 201) {
      canvas.setTextColor(TFT_GREEN);
      canvas.setCursor(10, 115);
      canvas.print("Cloud: OK");
    } else if (gSupabaseHttpCode == 0) {
      canvas.setTextColor(TFT_YELLOW);
      canvas.setCursor(10, 115);
      canvas.print("Cloud: Wait...");
    } else {
      canvas.setTextColor(TFT_RED);
      canvas.setCursor(10, 115);
      canvas.printf("Cloud: Err %d", gSupabaseHttpCode);
    }
  } else {
    canvas.setTextColor(TFT_RED);
    canvas.setCursor(10, 115);
    canvas.print("WiFi: Disconnected");
  }

  canvas.drawFastVLine(IMU_X, 0, LCD_H, TFT_DARKGREY);

  // --- 右側 1/3 (IMU) ---
  float ax, ay, az;
  M5.Imu.getAccel(&ax, &ay, &az);

  canvas.setTextSize(1);
  canvas.setTextColor(TFT_LIGHTGREY);
  canvas.setCursor(IMU_X + 5, 15);
  canvas.print("[IMU Accel]");

  canvas.setTextSize(1);
  canvas.setTextColor(TFT_WHITE);
  canvas.setCursor(IMU_X + 5, 40);
  canvas.printf("X: %.2f", ax);
  canvas.setCursor(IMU_X + 5, 55);
  canvas.printf("Y: %.2f", ay);
  canvas.setCursor(IMU_X + 5, 70);
  canvas.printf("Z: %.2f", az);

  canvas.pushSprite(0, 0);
}

// ================= 更新処理 =================
static void envTick() {
  if(!gEnvReady) return;
  Units.update();
  if (sht30.updated()) {
    gEnv_sht30_tempC = sht30.temperature();
    gEnv_sht30_humRH = sht30.humidity();
  }
  if (qmp6988.updated()) {
    gEnv_qmp6988_pressPa = qmp6988.pressure();
  }
}

// ===========================================

void setup(){
  Serial.begin(115200);
  delay(120);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.clear_display = false;
  cfg.internal_imu = true;
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(255);

  // 設定の読み込み
  prefs.begin("config", false);
  gDeviceId = prefs.getString("devid", gDeviceId);
  gDeviceName = prefs.getString("devname", gDeviceName);
  gSSID = prefs.getString("ssid", "");
  gPassword = prefs.getString("pass", "");
  gLat = prefs.getFloat("lat", gLat);
  gLon = prefs.getFloat("lon", gLon);

// --- 以下を追加：読み込んだ設定値のシリアル出力 ---
  Serial.println("\n--- Loaded Configuration ---");
  Serial.printf("Device ID  : %s\n", gDeviceId.c_str());
  Serial.printf("Device Name: %s\n", gDeviceName.c_str());
  Serial.printf("SSID       : %s\n", gSSID.c_str());
  
  // パスワードはそのまま出すか、伏せ字にするか運用に応じて調整
  Serial.printf("Password   : %s\n", gPassword.c_str()); 
  // 伏せ字にする場合の例: Serial.println("Password   : ********");
  
  Serial.printf("Latitude   : %.6f\n", gLat);
  Serial.printf("Longitude  : %.6f\n", gLon);
  Serial.println("----------------------------\n");
  // M5ボタン(BtnA)を押しながら起動するか、SSIDが空ならConfigモードに入る
  M5.update();
  if (gSSID == "" || M5.BtnA.isPressed()) {
    enterConfigMode();
    return; // Configモードの場合はsetupをここで終了（loopでWebサーバー処理へ）
  }

  // --- 以下、通常モード ---
  canvas.setColorDepth(16);
  canvas.createSprite(LCD_W, LCD_H);

  int pin_sda = 0;
  int pin_scl = 26;
  Wire.begin(pin_sda, pin_scl, 400000U); 
  gEnvReady = Units.add(unitENV3, Wire) && Units.begin();

// 既存の WiFi.mode(WIFI_STA); の直後に以下を追加・修正
  WiFi.mode(WIFI_STA);
  
  // DNSを手動指定（IP, Gateway, Subnetを0.0.0.0にすることでDHCPを維持）
  IPAddress primaryDNS(8, 8, 8, 8);
  IPAddress secondaryDNS(8, 8, 4, 4);
  IPAddress noIP(0, 0, 0, 0);
  WiFi.config(noIP, noIP, noIP, primaryDNS, secondaryDNS);
  
  WiFi.begin(gSSID.c_str(), gPassword.c_str());
  configTime(9 * 3600, 0, "pool.ntp.org");
}

void loop(){
  M5.update();

  // Configモード時の処理（Webサーバーの応答のみ行い、以降の処理はスキップ）
  if (gConfigMode) {
    server.handleClient();
    delay(10);
    return; 
  }

  // --- 以下、通常モード時の処理 ---

  // M5ボタン(BtnA)を2秒長押しで設定をリセットし、再起動(Configモードへ)
  if (M5.BtnA.pressedFor(2000)) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setCursor(10, 60);
    M5.Display.println("RESET CONFIG...");
    prefs.clear(); // 保存された設定を全消去
    delay(1000);
    ESP.restart(); // 再起動（次回起動時はSSIDが空のため自動でConfigモードになる）
  }

  // 右側面ボタン(BtnB)で画面反転
  if (M5.BtnB.wasPressed()) {
    static bool gInvert = false;
    gInvert = !gInvert;
    M5.Display.invertDisplay(gInvert);
  }

  uint32_t now = millis();
  checkWiFiConnection(now);
  envTick();

  drawScreen();

  // Supabaseへの送信判定
  if (gEnvReady && !isnan(gEnv_sht30_tempC) && (now - gLastSupabaseSendMs) >= SUPABASE_INTERVAL_MS) {
    gLastSupabaseSendMs = now;
    float currentWbgt = calculateWBGT(gEnv_sht30_tempC, gEnv_sht30_humRH);
    sendToSupabase(gEnv_sht30_tempC, currentWbgt);
  }

  delay(20);
}