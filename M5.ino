#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>

// ================== WiFi settings ==================
#define WIFI_SSID "Forsberg"
#define WIFI_PASS "CommSucks"

// ================== Buzzer settings ==================
#define BUZZER_PIN 2

// ================== Pi server settings ==================
#define PI_HOST "172.20.10.10"   // <-- CHANGE THIS to Pi IP (or hostname)
#define PI_PORT 5050

// ================== Time / NTP (Central Time) ==================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -6 * 3600;
const int daylightOffset_sec = 3600;

WebServer server(80);
bool wifiOk = false;
bool timeOk = false;

// Alarm state
bool alarmSet = false;
time_t alarmEpoch = 0;
char alarmLabel[96] = {0};

bool buzzerReady = false;
bool showingTime = false;
unsigned long lastTimeUpdateMs = 0;

unsigned long lastNextPollMs = 0;
const unsigned long NEXT_POLL_INTERVAL_MS = 15000; // 15s

// ---------- Battery helpers ----------
int getBatteryPercent() {
  float v = M5.Axp.GetBatVoltage();
  const float minV = 3.30;
  const float maxV = 4.15;
  int pct = (int)((v - minV) * 100.0f / (maxV - minV));
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void drawBatteryIconTopRight(int percent) {
  const int iconW = 26;
  const int iconH = 12;
  const int x = 240 - iconW - 6;
  const int y = 4;

  M5.Lcd.drawRect(x, y, iconW, iconH, WHITE);
  M5.Lcd.fillRect(x + iconW, y + 3, 3, iconH - 6, WHITE);

  int innerW = iconW - 4;
  int innerH = iconH - 4;
  int fillW = innerW * percent / 100;

  M5.Lcd.fillRect(x + 2, y + 2, innerW, innerH, BLACK);

  if (fillW > 0) {
    uint16_t color = (percent < 20) ? RED : GREEN;
    M5.Lcd.fillRect(x + 2, y + 2, fillW, innerH, color);
  }

  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  M5.Lcd.setCursor(x - 30, y + 1);
  M5.Lcd.print(buf);
}

void showCurrentTimeOnScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(5, 30);
    M5.Lcd.println("Time not set");
    M5.Lcd.setTextSize(1);
    M5.Lcd.setCursor(5, 60);
    M5.Lcd.println("Check WiFi/NTP");
    drawBatteryIconTopRight(getBatteryPercent());
    return;
  }

  char timeStr[32];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 30);
  M5.Lcd.println(timeStr);

  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(10, 70);
  M5.Lcd.println("Local time (CST/CDT)");

  M5.Lcd.setCursor(10, 90);
  if (alarmSet) {
    M5.Lcd.print("Next: ");
    M5.Lcd.println(alarmLabel[0] ? alarmLabel : "(scheduled)");
  } else {
    M5.Lcd.println("Next: (none)");
  }

  drawBatteryIconTopRight(getBatteryPercent());
}

void buzzerAlert() {
  if (!buzzerReady) return;
  unsigned long endTime = millis() + 10000;
  while (millis() < endTime) {
    ledcWriteTone(BUZZER_PIN, 2000);
    delay(250);
    ledcWriteTone(BUZZER_PIN, 0);
    delay(150);
  }
  ledcWriteTone(BUZZER_PIN, 0);
}

String piBaseUrl() {
  return String("http://") + PI_HOST + ":" + String(PI_PORT);
}

bool httpGetFromPi(const String& path, String& outBody, String& outCT) {
  HTTPClient http;
  String url = piBaseUrl() + path;
  http.begin(url);
  int code = http.GET();
  if (code <= 0) { http.end(); return false; }
  outBody = http.getString();
  outCT = http.header("Content-Type");
  http.end();
  return (code >= 200 && code < 300);
}

bool httpPostToPiText(const String& path, const String& bodyIn, String& outBody) {
  HTTPClient http;
  String url = piBaseUrl() + path;
  http.begin(url);
  http.addHeader("Content-Type", "text/plain");
  int code = http.POST((uint8_t*)bodyIn.c_str(), bodyIn.length());
  if (code <= 0) { http.end(); return false; }
  outBody = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

// Tiny JSON helpers for /api/next response
long jsonGetLong(const String& body, const char* key) {
  String k = String("\"") + key + "\":";
  int idx = body.indexOf(k);
  if (idx < 0) return -1;
  idx += k.length();
  while (idx < (int)body.length() && body[idx] == ' ') idx++;
  int end = idx;
  while (end < (int)body.length() && (isDigit(body[end]) || body[end] == '-')) end++;
  return body.substring(idx, end).toInt();
}

String jsonGetString(const String& body, const char* key) {
  String k = String("\"") + key + "\":";
  int idx = body.indexOf(k);
  if (idx < 0) return "";
  idx += k.length();
  while (idx < (int)body.length() && body[idx] != '"') idx++;
  if (idx >= (int)body.length()) return "";
  idx++;
  int end = body.indexOf('"', idx);
  if (end < 0) return "";
  return body.substring(idx, end);
}

void refreshNextFromPi() {
  if (!wifiOk) return;

  String body, ct;
  if (!httpGetFromPi("/api/next", body, ct)) {
    alarmSet = false;
    alarmEpoch = 0;
    alarmLabel[0] = 0;
    return;
  }

  long nextEpoch = jsonGetLong(body, "next_epoch");
  String nextLabel = jsonGetString(body, "next_label");

  if (nextEpoch > 0) {
    alarmSet = true;
    alarmEpoch = (time_t)nextEpoch;
    snprintf(alarmLabel, sizeof(alarmLabel), "%s", nextLabel.c_str());
  } else {
    alarmSet = false;
    alarmEpoch = 0;
    alarmLabel[0] = 0;
  }
}

// ===== Web UI (hosted on M5, backed by Pi via proxy) =====
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>APD Scheduler</title>
  <style>
    body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:linear-gradient(135deg,#0f172a,#1d4ed8);color:#e5e7eb;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:18px;}
    .card{background:rgba(15,23,42,.92);border-radius:16px;padding:20px 18px;box-shadow:0 20px 40px rgba(0,0,0,.4);max-width:760px;width:100%;}
    h1{margin:0 0 6px;font-size:1.3rem;}
    .sub{color:#9ca3af;margin:0 0 16px;}
    .row{display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin-bottom:12px;}
    .pill{padding:6px 10px;border:1px solid rgba(255,255,255,.15);border-radius:999px;color:#a5b4fc;font-size:.9rem}
    table{width:100%;border-collapse:collapse;margin-top:10px;}
    th,td{border-bottom:1px solid rgba(255,255,255,.08);padding:10px 8px;text-align:left;}
    th{color:#cbd5e1;font-weight:600;}
    input{width:90px;padding:8px 10px;border-radius:10px;border:1px solid #4b5563;background:#020617;color:#e5e7eb;}
    button{background:#6366f1;color:white;border:none;border-radius:999px;padding:9px 14px;font-size:.95rem;cursor:pointer}
    button:hover{background:#4f46e5}
    .status{margin-top:10px;color:#9ca3af;font-size:.9rem;white-space:pre-wrap}
    .ok{color:#86efac}
    .bad{color:#fca5a5}
  </style>
</head>
<body>
  <div class="card">
    <h1>APD Scheduler (Synced)</h1>
    <p class="sub">This page is hosted on the M5, but schedules are stored on the Raspberry Pi.</p>

    <div class="row">
      <div class="pill">M5: <span id="m5"></span></div>
      <div class="pill">Pi: PI_HOST:PI_PORT</div>
      <button onclick="loadSched()">Refresh</button>
      <button onclick="saveSched()">Save</button>
    </div>

    <div class="row">
      <div class="pill">Next dispense: <span id="nextdisp">(loading)</span></div>
      <button onclick="refreshNext()">Refresh Next</button>
    </div>

    <table>
      <thead><tr><th>Day</th><th>Slot 1</th><th>Slot 2</th></tr></thead>
      <tbody id="tbody"></tbody>
    </table>

    <div id="status" class="status"></div>
  </div>

<script>
const DAYS = ["Mon","Tue","Wed","Thu","Fri","Sat","Sun"];

function setStatus(msg, ok=true){
  const el = document.getElementById("status");
  el.textContent = msg || "";
  el.className = "status " + (ok ? "ok" : "bad");
}

function buildTableFromText(txt){
  const map = {};
  for(const d of DAYS) map[d] = {slot1:"08:00", slot2:"20:00"};
  const lines = (txt||"").split("\n").map(l=>l.trim()).filter(Boolean);
  for(const line of lines){
    const parts = line.split(/\s+/);
    if(parts.length >= 3 && map[parts[0]]){
      map[parts[0]].slot1 = parts[1];
      map[parts[0]].slot2 = parts[2];
    }
  }

  const tb = document.getElementById("tbody");
  tb.innerHTML = "";
  for(const d of DAYS){
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${d}</td>
      <td><input id="${d}_s1" value="${map[d].slot1}" /></td>
      <td><input id="${d}_s2" value="${map[d].slot2}" /></td>
    `;
    tb.appendChild(tr);
  }
}

function tableToText(){
  let out = "";
  for(const d of DAYS){
    const s1 = document.getElementById(`${d}_s1`).value.trim();
    const s2 = document.getElementById(`${d}_s2`).value.trim();
    out += `${d} ${s1} ${s2}\n`;
  }
  return out;
}

async function loadSched(){
  setStatus("Loading schedule from Pi...");
  try{
    const r = await fetch("/api/schedule");
    const t = await r.text();
    if(!r.ok) throw new Error(t || "Failed");
    buildTableFromText(t);
    setStatus("Loaded schedule.", true);
  }catch(e){
    setStatus("Load failed: " + e.message, false);
  }
}

async function saveSched(){
  setStatus("Saving schedule to Pi...");
  try{
    const body = tableToText();
    const r = await fetch("/api/schedule", {
      method:"POST",
      headers:{"Content-Type":"text/plain"},
      body
    });
    const t = await r.text();
    if(!r.ok) throw new Error(t || "Failed");
    setStatus("Saved schedule to Pi.", true);
    await refreshNext();
  }catch(e){
    setStatus("Save failed: " + e.message, false);
  }
}

async function refreshNext(){
  try{
    const r = await fetch("/api/next");
    const j = await r.json();
    if(j.ok && j.next_label){
      document.getElementById("nextdisp").textContent = j.next_label;
    }else{
      document.getElementById("nextdisp").textContent = "(none)";
    }
  }catch(e){
    document.getElementById("nextdisp").textContent = "(error)";
  }
}

document.getElementById("m5").textContent = location.host;
loadSched();
refreshNext();
</script>
</body>
</html>
)rawliteral";

  html.replace("PI_HOST", String(PI_HOST));
  html.replace("PI_PORT", String(PI_PORT));

  server.send(200, "text/html", html);
}

// ---- Proxy routes (browser talks to M5, M5 talks to Pi) ----
void handleApiScheduleGet() {
  String body, ct;
  if (!httpGetFromPi("/api/schedule", body, ct)) {
    server.send(502, "text/plain", "Pi unreachable\n");
    return;
  }
  server.send(200, "text/plain", body);
}

void handleApiSchedulePost() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "Missing body\n");
    return;
  }
  String in = server.arg("plain");
  String out;
  if (!httpPostToPiText("/api/schedule", in, out)) {
    server.send(502, "text/plain", "Pi write failed\n");
    return;
  }
  server.send(200, "text/plain", out);
  refreshNextFromPi();
}

void handleApiNext() {
  String body, ct;
  if (!httpGetFromPi("/api/next", body, ct)) {
    server.send(502, "application/json", "{\"ok\":false,\"error\":\"Pi unreachable\"}");
    return;
  }
  server.send(200, "application/json", body);
}

// ========== WiFi + NTP setup ==========
void setupWiFiAndTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(5, 5);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Connecting...");
  Serial.print("Connecting to WiFi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
    M5.Lcd.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiOk = true;
    IPAddress ip = WiFi.localIP();

    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(5, 5);
    M5.Lcd.println("WiFi OK");
    M5.Lcd.setCursor(5, 25);
    M5.Lcd.println(ip.toString());

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    for (int i = 0; i < 20; ++i) {
      delay(500);
      if (getLocalTime(&timeinfo)) {
        timeOk = true;
        break;
      }
    }
  } else {
    wifiOk = false;
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(5, 5);
    M5.Lcd.println("WiFi FAIL");
  }
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);

  setupWiFiAndTime();

  buzzerReady = ledcAttach(BUZZER_PIN, 2000, 8);

  if (wifiOk) {
    server.on("/", handleRoot);
    server.on("/api/schedule", HTTP_GET, handleApiScheduleGet);
    server.on("/api/schedule", HTTP_POST, handleApiSchedulePost);
    server.on("/api/next", HTTP_GET, handleApiNext);
    server.begin();
  }

  showingTime = true;
  refreshNextFromPi();
  showCurrentTimeOnScreen();
  lastTimeUpdateMs = millis();
  lastNextPollMs = millis();
}

void loop() {
  M5.update();

  if (wifiOk) server.handleClient();

  if (showingTime && timeOk) {
    unsigned long nowMs = millis();
    if (nowMs - lastTimeUpdateMs >= 1000) {
      lastTimeUpdateMs = nowMs;
      showCurrentTimeOnScreen();
    }
  }

  if (wifiOk && (millis() - lastNextPollMs >= NEXT_POLL_INTERVAL_MS)) {
    lastNextPollMs = millis();
    refreshNextFromPi();
  }

  if (alarmSet && timeOk) {
    struct tm nowInfo;
    if (getLocalTime(&nowInfo)) {
      time_t nowEpoch = mktime(&nowInfo);
      if (nowEpoch >= alarmEpoch) {
        alarmSet = false;

        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(5, 5);
        M5.Lcd.setTextSize(3);
        M5.Lcd.println("DISPENSE!");
        drawBatteryIconTopRight(getBatteryPercent());

        buzzerAlert();

        refreshNextFromPi();
        showCurrentTimeOnScreen();
      }
    }
  }
}

