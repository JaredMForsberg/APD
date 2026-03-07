#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>

#define WIFI_SSID "Forsberg"
#define WIFI_PASS "CommSucks"

#define BUZZER_PIN 2

#define PI_HOST "172.20.10.10"
#define PI_PORT 5050

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -6 * 3600;
const int daylightOffset_sec = 3600;

WebServer server(80);
bool wifiOk = false;
bool timeOk = false;

bool alarmSet = false;
time_t alarmEpoch = 0;
char alarmLabel[96] = {0};

bool buzzerReady = false;
bool showingTime = false;
unsigned long lastTimeUpdateMs = 0;
unsigned long lastNextPollMs = 0;
const unsigned long NEXT_POLL_INTERVAL_MS = 15000;

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
  http.begin(piBaseUrl() + path);
  int code = http.GET();
  if (code <= 0) { http.end(); return false; }
  outBody = http.getString();
  outCT = http.header("Content-Type");
  http.end();
  return (code >= 200 && code < 300);
}

bool httpPostToPiText(const String& path, const String& bodyIn, String& outBody) {
  HTTPClient http;
  http.begin(piBaseUrl() + path);
  http.addHeader("Content-Type", "text/plain");
  int code = http.POST((uint8_t*)bodyIn.c_str(), bodyIn.length());
  if (code <= 0) { http.end(); return false; }
  outBody = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

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

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>APD Scheduler</title>
  <style>
    :root{
      --bg1:#0f172a;
      --bg2:#1d4ed8;
      --card:#0f172aee;
      --muted:#94a3b8;
      --line:rgba(255,255,255,.10);
      --accent:#6366f1;
      --accent2:#4f46e5;
      --ok:#86efac;
      --bad:#fca5a5;
      --text:#e5e7eb;
    }
    *{box-sizing:border-box}
    body{
      margin:0;
      font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
      background:linear-gradient(135deg,var(--bg1),var(--bg2));
      color:var(--text);
      min-height:100vh;
      display:flex;
      align-items:center;
      justify-content:center;
      padding:18px;
    }
    .card{
      background:var(--card);
      border-radius:18px;
      padding:20px 18px;
      box-shadow:0 20px 40px rgba(0,0,0,.4);
      max-width:760px;
      width:100%;
    }
    h1{margin:0 0 6px;font-size:1.35rem}
    .sub{color:var(--muted);margin:0 0 16px}
    .row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:12px;}
    .pill{
      padding:6px 10px;
      border:1px solid rgba(255,255,255,.15);
      border-radius:999px;
      color:#c7d2fe;
      font-size:.9rem
    }
    button, select{
      background:var(--accent);
      color:white;
      border:none;
      border-radius:999px;
      padding:9px 14px;
      font-size:.95rem;
      cursor:pointer
    }
    button:hover, select:hover{background:var(--accent2)}
    .ghost{background:#1e293b}
    .daybar{
      display:flex;
      justify-content:space-between;
      align-items:center;
      gap:10px;
      margin:14px 0 10px;
      flex-wrap:wrap;
    }
    .section{
      border:1px solid var(--line);
      border-radius:14px;
      padding:14px;
      margin-top:12px;
      background:rgba(2,6,23,.45);
    }
    .section h2{margin:0 0 10px;font-size:1.05rem}
    .slot-top{
      display:flex;
      align-items:center;
      justify-content:space-between;
      gap:10px;
      margin-bottom:10px;
      flex-wrap:wrap;
    }
    .slot-controls{display:flex;gap:8px;align-items:center}
    .time-row{
      display:flex;
      align-items:center;
      gap:10px;
      margin-bottom:8px;
      padding:8px 0;
      border-bottom:1px solid rgba(255,255,255,.06);
    }
    .time-row:last-child{border-bottom:none}
    .idx{width:24px;color:var(--muted);font-size:.9rem}
    input[type="time"]{
      padding:8px 10px;
      border-radius:10px;
      border:1px solid #475569;
      background:#020617;
      color:var(--text);
      width:140px;
    }
    .empty{color:var(--muted);font-size:.95rem;padding:4px 0}
    .status{margin-top:10px;color:var(--muted);font-size:.9rem;white-space:pre-wrap}
    .ok{color:var(--ok)}
    .bad{color:var(--bad)}
  </style>
</head>
<body>
  <div class="card">
    <h1>APD Scheduler</h1>
    <p class="sub">Hosted on the M5. Schedules are synced with the Raspberry Pi.</p>

    <div class="row">
      <div class="pill">M5: <span id="m5"></span></div>
      <div class="pill">Pi: __PI_HOST__:__PI_PORT__</div>
      <button onclick="loadSched()">Refresh</button>
      <button onclick="saveSched()">Save</button>
    </div>

    <div class="row">
      <div class="pill">Next dispense: <span id="nextdisp">(loading)</span></div>
      <button class="ghost" onclick="refreshNext()">Refresh Next</button>
    </div>

    <div class="daybar">
      <div class="pill">Editing day: <span id="currentDayLabel"></span></div>
      <select id="daySelect" onchange="renderDay()"></select>
    </div>

    <div id="dayContainer"></div>
    <div id="status" class="status"></div>
  </div>

<script>
const DAYS = ["Mon","Tue","Wed","Thu","Fri","Sat","Sun"];
const MAX_TIMES = 10;

function blankEntry(defTime){
  return { enabled: 0, time: defTime };
}

function blankDay(day){
  return {
    day,
    slot1: Array.from({length: MAX_TIMES}, () => blankEntry("08:00")),
    slot2: Array.from({length: MAX_TIMES}, () => blankEntry("20:00"))
  };
}

let scheduleData = DAYS.map(d => blankDay(d));

function setStatus(msg, ok=true){
  const el = document.getElementById("status");
  el.textContent = msg || "";
  el.className = "status " + (ok ? "ok" : "bad");
}

function getTodayDayName(){
  const jsDay = new Date().getDay();
  const map = ["Sun","Mon","Tue","Wed","Thu","Fri","Sat"];
  return map[jsDay];
}

function ensureDaySelect(){
  const sel = document.getElementById("daySelect");
  sel.innerHTML = "";
  for(const d of DAYS){
    const opt = document.createElement("option");
    opt.value = d;
    opt.textContent = d;
    sel.appendChild(opt);
  }
  const today = getTodayDayName();
  sel.value = DAYS.includes(today) ? today : "Mon";
  document.getElementById("currentDayLabel").textContent = sel.value;
}

function parseScheduleText(txt){
  const out = DAYS.map(d => blankDay(d));
  const lines = (txt || "").split("\n").map(x => x.trim()).filter(Boolean);

  for(const line of lines){
    const parts = line.split(/\s+/);
    if(parts.length < 22) continue;

    const day = parts[0];
    const obj = out.find(x => x.day === day);
    if(!obj) continue;

    let idx = 1;
    for(let i = 0; i < MAX_TIMES; i++){
      const p = parts[idx++];
      if(!p) break;
      const m = p.match(/^(\d):(\d{2}):(\d{2})$/);
      if(m){
        obj.slot1[i].enabled = parseInt(m[1], 10) ? 1 : 0;
        obj.slot1[i].time = `${m[2]}:${m[3]}`;
      }
    }

    if(parts[idx] === "|") idx++;

    for(let i = 0; i < MAX_TIMES; i++){
      const p = parts[idx++];
      if(!p) break;
      const m = p.match(/^(\d):(\d{2}):(\d{2})$/);
      if(m){
        obj.slot2[i].enabled = parseInt(m[1], 10) ? 1 : 0;
        obj.slot2[i].time = `${m[2]}:${m[3]}`;
      }
    }
  }

  return out;
}

function scheduleToText(){
  let out = "";
  for(const d of scheduleData){
    out += d.day + " ";
    for(let i = 0; i < MAX_TIMES; i++){
      const e = d.slot1[i];
      out += `${e.enabled ? 1 : 0}:${e.time} `;
    }
    out += "| ";
    for(let i = 0; i < MAX_TIMES; i++){
      const e = d.slot2[i];
      out += `${e.enabled ? 1 : 0}:${e.time} `;
    }
    out += "\n";
  }
  return out;
}

function activeCount(arr){
  return arr.filter(x => x.enabled).length;
}

function visibleCount(arr){
  return activeCount(arr);
}

function sortSlot(arr, defTime){
  const enabled = arr
    .filter(x => x.enabled)
    .sort((a,b) => {
      const am = parseInt(a.time.slice(0,2),10) * 60 + parseInt(a.time.slice(3,5),10);
      const bm = parseInt(b.time.slice(0,2),10) * 60 + parseInt(b.time.slice(3,5),10);
      return am - bm;
    });

  const disabled = Array.from({length: MAX_TIMES - enabled.length}, () => blankEntry(defTime));
  return [...enabled, ...disabled];
}

function getCurrentDayObj(){
  const sel = document.getElementById("daySelect").value;
  return scheduleData.find(x => x.day === sel);
}

function addRow(slotName){
  const day = getCurrentDayObj();
  const arr = day[slotName];
  const count = activeCount(arr);
  if(count >= MAX_TIMES) return;

  const defTime = (slotName === "slot1") ? "08:00" : "20:00";
  arr[count].enabled = 1;
  if(!arr[count].time) arr[count].time = defTime;

  renderDay();
}

function removeRow(slotName){
  const day = getCurrentDayObj();
  const arr = day[slotName];
  const count = activeCount(arr);
  if(count <= 0){
    renderDay();
    return;
  }

  arr[count - 1].enabled = 0;

  const defTime = (slotName === "slot1") ? "08:00" : "20:00";
  day[slotName] = sortSlot(arr, defTime);

  renderDay();
}

function updateTime(slotName, index, value){
  const day = getCurrentDayObj();
  day[slotName][index].time = value;
  day[slotName][index].enabled = 1;
}

function renderSlot(title, slotName){
  const day = getCurrentDayObj();
  const defTime = (slotName === "slot1") ? "08:00" : "20:00";
  day[slotName] = sortSlot(day[slotName], defTime);

  const arr = day[slotName];
  const vis = visibleCount(arr);
  const active = activeCount(arr);

  let rows = "";
  if(active === 0){
    rows = `<div class="empty">No time slots set</div>`;
  } else {
    for(let i = 0; i < vis; i++){
      const t = arr[i].time || defTime;
      rows += `
        <div class="time-row">
          <div class="idx">${i + 1}.</div>
          <input type="time" value="${t}" onchange="updateTime('${slotName}', ${i}, this.value)" />
        </div>
      `;
    }
  }

  return `
    <div class="section">
      <div class="slot-top">
        <h2>${title}</h2>
        <div class="slot-controls">
          <button class="ghost" onclick="removeRow('${slotName}')">-</button>
          <button onclick="addRow('${slotName}')">+</button>
        </div>
      </div>
      ${rows}
    </div>
  `;
}

function renderDay(){
  const sel = document.getElementById("daySelect").value;
  document.getElementById("currentDayLabel").textContent = sel;
  document.getElementById("dayContainer").innerHTML =
    renderSlot("Pill Slot 1", "slot1") +
    renderSlot("Pill Slot 2", "slot2");
}

async function loadSched(){
  setStatus("Loading schedule from Pi...");
  try{
    const r = await fetch("/api/schedule");
    const t = await r.text();
    if(!r.ok) throw new Error(t || "Failed");
    scheduleData = parseScheduleText(t);
    renderDay();
    setStatus("Loaded schedule.", true);
  }catch(e){
    setStatus("Load failed: " + e.message, false);
  }
}

async function saveSched(){
  setStatus("Saving schedule to Pi...");
  try{
    const body = scheduleToText();
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
ensureDaySelect();
renderDay();
loadSched();
refreshNext();
</script>
</body>
</html>
)rawliteral";

  html.replace("__PI_HOST__", String(PI_HOST));
  html.replace("__PI_PORT__", String(PI_PORT));

  server.send(200, "text/html", html);
}

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

void setupWiFiAndTime() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setCursor(5, 5);
  M5.Lcd.setTextSize(2);
  M5.Lcd.println("Connecting...");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    M5.Lcd.print(".");
  }

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
