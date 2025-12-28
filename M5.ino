#include <M5StickCPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>

// ================== WiFi settings ==================
#define WIFI_SSID "Forsberg"
#define WIFI_PASS "CommSucks"

// ================== Buzzer settings ==================
#define BUZZER_PIN 2               // Buzzer pin on M5StickC Plus (GPIO2)

// ================== Time / NTP (Central Time) ==================
const char* ntpServer = "pool.ntp.org";
// Central time: UTC-6, with +1h for DST
const long gmtOffset_sec = -6 * 3600;
const int daylightOffset_sec = 3600;

WebServer server(80);
bool wifiOk = false;
bool timeOk = false;               // set true once NTP time is obtained

// Alarm state (scheduler based on CST/CDT time)
bool alarmSet = false;
time_t alarmEpoch = 0;             // absolute local time when alarm should fire
int alarmHour = -1;                // scheduled hour (0–23)
int alarmMinute = -1;              // scheduled minute (0–59)

// Buzzer state (for LEDC)
bool buzzerReady = false;

// Clock display state
bool showingTime = false;
unsigned long lastTimeUpdateMs = 0;

// ---------- Battery helpers (M5StickC Plus AXP192) ----------
int getBatteryPercent() {
  // Read battery voltage from AXP192
  float v = M5.Axp.GetBatVoltage();  // usually ~3.3V (low) to ~4.15V (full)

  // Rough mapping voltage -> percentage
  const float minV = 3.30;  // "empty"
  const float maxV = 4.15;  // "full"
  int pct = (int)((v - minV) * 100.0f / (maxV - minV));

  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void drawBatteryIconTopRight(int percent) {
  // Screen is 240x135 in rotation(1)
  const int iconW = 26;
  const int iconH = 12;
  const int x = 240 - iconW - 6;  // margin from right
  const int y = 4;

  // Outline
  M5.Lcd.drawRect(x, y, iconW, iconH, WHITE);
  // Tip
  M5.Lcd.fillRect(x + iconW, y + 3, 3, iconH - 6, WHITE);

  // Fill level
  int innerW = iconW - 4;
  int innerH = iconH - 4;
  int fillW = innerW * percent / 100;

  // Clear inner first
  M5.Lcd.fillRect(x + 2, y + 2, innerW, innerH, BLACK);

  if (fillW > 0) {
    uint16_t color = (percent < 20) ? RED : GREEN;
    M5.Lcd.fillRect(x + 2, y + 2, fillW, innerH, color);
  }

  // Percentage text (small, left of icon)
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE, BLACK);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  M5.Lcd.setCursor(x - 30, y + 1);
  M5.Lcd.print(buf);
}

// ========== Show current local CST/CDT time on the watch ==========
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

    // Show battery even if time not set
    int pct = getBatteryPercent();
    drawBatteryIconTopRight(pct);
    return;
  }

  char timeStr[32];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);

  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 40);
  M5.Lcd.println(timeStr);

  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(10, 80);
  M5.Lcd.println("Local time (CST/CDT)");

  // Battery in top-right
  int pct = getBatteryPercent();
  drawBatteryIconTopRight(pct);
}

// ========== Buzzer alert: beep for about 10 seconds ==========
void buzzerAlert() {
  if (!buzzerReady) {
    Serial.println("Buzzer not attached, skipping sound.");
    return;
  }

  unsigned long endTime = millis() + 10000;  // 10 seconds

  while (millis() < endTime) {
    // Tone ON
    ledcWriteTone(BUZZER_PIN, 2000);  // 2kHz
    delay(250);

    // Tone OFF
    ledcWriteTone(BUZZER_PIN, 0);
    delay(150);
  }

  ledcWriteTone(BUZZER_PIN, 0);
}

// ========== HTTP: root page with scheduler UI ==========
void handleRoot() {
  long remainingSec = 0;
  char schedStr[32] = "";
  bool haveSchedStr = false;

  if (alarmSet && timeOk) {
    struct tm nowInfo;
    if (getLocalTime(&nowInfo)) {
      time_t nowEpoch = mktime(&nowInfo);
      long diff = (long)(alarmEpoch - nowEpoch);
      if (diff > 0) {
        remainingSec = diff;
      } else {
        remainingSec = 0;
      }

      struct tm *alarmInfo = localtime(&alarmEpoch);
      if (alarmInfo != nullptr) {
        strftime(schedStr, sizeof(schedStr), "%Y-%m-%d %H:%M:%S", alarmInfo);
        haveSchedStr = true;
      }
    }
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>APD Scheduler</title>
  <style>
    body {
      margin: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: linear-gradient(135deg, #0f172a, #1d4ed8);
      color: #e5e7eb;
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
    }
    .card {
      background: rgba(15, 23, 42, 0.9);
      border-radius: 16px;
      padding: 24px 28px;
      box-shadow: 0 20px 40px rgba(0,0,0,0.4);
      max-width: 420px;
      width: 100%;
      box-sizing: border-box;
    }
    h1 {
      margin: 0 0 8px;
      font-size: 1.4rem;
    }
    p {
      margin: 4px 0;
      color: #9ca3af;
    }
    .ip {
      margin-bottom: 16px;
      font-size: 0.9rem;
      color: #a5b4fc;
    }
    label {
      display: block;
      margin-bottom: 4px;
      font-size: 0.9rem;
      color: #e5e7eb;
    }
    input[type="number"] {
      width: 100%;
      padding: 8px 10px;
      border-radius: 8px;
      border: 1px solid #4b5563;
      background: #020617;
      color: #e5e7eb;
      box-sizing: border-box;
      margin-bottom: 10px;
      font-size: 0.95rem;
    }
    input[type="submit"] {
      background: #6366f1;
      color: white;
      border: none;
      border-radius: 999px;
      padding: 8px 16px;
      font-size: 0.95rem;
      cursor: pointer;
      transition: background 0.15s ease, transform 0.1s ease;
    }
    input[type="submit"]:hover {
      background: #4f46e5;
      transform: translateY(-1px);
    }
    .schedule {
      margin-top: 16px;
      padding: 10px 12px;
      border-radius: 10px;
      background: rgba(15, 118, 110, 0.15);
      border: 1px solid rgba(45, 212, 191, 0.4);
      font-weight: 500;
      font-size: 0.95rem;
    }
    .status {
      margin-top: 8px;
      font-size: 0.9rem;
      color: #9ca3af;
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>APD Scheduler</h1>
    <p class="ip">Device IP: )rawliteral";

  html += WiFi.localIP().toString();
  html += R"rawliteral(</p>
    <p>Set a specific Central Time (CST/CDT) for the alarm. The watch uses NTP-synced local time.</p>
    <form action="/set" method="GET">
      <label for="hour">Hour (0–23, CST/CDT)</label>
      <input type="number" id="hour" name="hour" min="0" max="23" value="8" required>

      <label for="minute">Minute (0–59)</label>
      <input type="number" id="minute" name="minute" min="0" max="59" value="0" required>

      <input type="submit" value="Schedule Alarm">
    </form>
)rawliteral";

  if (alarmSet && haveSchedStr) {
    html += R"rawliteral(
    <div class="schedule">
      <div>Alarm scheduled for: )rawliteral";
    html += String(schedStr);
    html += R"rawliteral( (CST/CDT)</div>
)rawliteral";

    long rem = remainingSec;
    long remMin = rem / 60;
    long remSec = rem % 60;

    html += R"rawliteral(
      <div>Approx. time remaining: )rawliteral";
    html += String(remMin);
    html += " min ";
    html += String(remSec);
    html += R"rawliteral( sec</div>
    </div>
    <p class="status">The APD will alarm on the M5StickC Plus at the scheduled Central Time.</p>
)rawliteral";
  } else {
    html += R"rawliteral(
    <p class="status">No alarm is currently scheduled. Choose a Central Time and schedule a new alarm.</p>
)rawliteral";
  }

  html += R"rawliteral(
  </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// ========== HTTP: /set?hour=HH&minute=MM ==========
void handleSetAlarm() {
  if (!server.hasArg("hour") || !server.hasArg("minute")) {
    server.send(400, "text/plain", "Missing 'hour' or 'minute' parameter");
    return;
  }

  int hour = server.arg("hour").toInt();
  int minute = server.arg("minute").toInt();

  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    server.send(400, "text/plain", "Invalid time");
    return;
  }

  struct tm nowInfo;
  if (!getLocalTime(&nowInfo)) {
    server.send(500, "text/plain", "Time not available (NTP error)");
    return;
  }

  time_t nowEpoch = mktime(&nowInfo);

  // Build target alarm time (today at hour:minute:00)
  struct tm target = nowInfo;
  target.tm_hour = hour;
  target.tm_min = minute;
  target.tm_sec = 0;

  time_t targetEpoch = mktime(&target);

  // If that time has already passed today, schedule for tomorrow
  if (targetEpoch <= nowEpoch) {
    targetEpoch += 24 * 3600;
  }

  alarmEpoch = targetEpoch;
  alarmSet = true;
  alarmHour = hour;
  alarmMinute = minute;

  char buf[64];
  struct tm *alarmInfo = localtime(&alarmEpoch);
  if (alarmInfo != nullptr) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", alarmInfo);
    Serial.print("Alarm scheduled for local time: ");
    Serial.println(buf);
  } else {
    Serial.println("Alarm scheduled (time conversion failed).");
  }

  // Redirect back to main UI
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Alarm scheduled, redirecting...");
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

    Serial.println("WiFi connected");
    Serial.print("IP: ");
    Serial.println(ip);

    // Configure NTP for local time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // Try to get time a few times so we don't hit "Time not set" forever
    struct tm timeinfo;
    for (int i = 0; i < 20; ++i) {   // ~10 seconds max
      delay(500);
      if (getLocalTime(&timeinfo)) {
        timeOk = true;
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.print("Time synced: ");
        Serial.println(buf);
        break;
      }
    }
    if (!timeOk) {
      Serial.println("Failed to sync time via NTP.");
    }
  } else {
    wifiOk = false;
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setCursor(5, 5);
    M5.Lcd.println("WiFi FAIL");
    Serial.println("WiFi connect failed");
  }
}

// ========== Arduino setup ==========
void setup() {
  M5.begin();
  Serial.begin(115200);

  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setTextSize(2);

  // Set up WiFi + NTP
  setupWiFiAndTime();

  // Attach LEDC to buzzer pin: 2 kHz, 8-bit resolution
  buzzerReady = ledcAttach(BUZZER_PIN, 2000, 8);
  if (!buzzerReady) {
    Serial.println("Failed to attach LEDC to buzzer pin!");
  }

  if (wifiOk) {
    server.on("/", handleRoot);
    server.on("/set", handleSetAlarm);
    server.begin();
    Serial.println("HTTP server started");
  }

  // Start in clock mode once time is available
  showingTime = true;
  showCurrentTimeOnScreen();
  lastTimeUpdateMs = millis();
}

// ========== Arduino loop ==========
void loop() {
  M5.update();

  if (wifiOk) {
    server.handleClient();
  }

  // If we're in clock mode, update the time every second
  if (showingTime && timeOk) {
    unsigned long nowMs = millis();
    if (nowMs - lastTimeUpdateMs >= 1000) {
      lastTimeUpdateMs = nowMs;
      showCurrentTimeOnScreen();
    }
  }

  // Check if alarm should fire based on scheduled CST/CDT time
  if (alarmSet && timeOk) {
    struct tm nowInfo;
    if (getLocalTime(&nowInfo)) {
      time_t nowEpoch = mktime(&nowInfo);
      if (nowEpoch >= alarmEpoch) {
        alarmSet = false;          // clear alarm so it doesn't retrigger
        showingTime = false;       // stop clock updates while we show ALARM

        // Show ALARM first
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setCursor(5, 5);
        M5.Lcd.setTextSize(3);
        M5.Lcd.println("ALARM!");

        // Battery on ALARM screen too
        int pct = getBatteryPercent();
        drawBatteryIconTopRight(pct);

        // Buzz for ~10 seconds
        buzzerAlert();

        // After alarm finishes, switch into clock mode and auto-update
        showCurrentTimeOnScreen();
        showingTime = true;
        lastTimeUpdateMs = millis();
      }
    }
  }
}

