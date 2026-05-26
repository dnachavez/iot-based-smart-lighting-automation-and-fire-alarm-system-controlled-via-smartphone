/*
  arduino.ino
  IoT-Based Smart Lighting Automation and Fire Alarm System
  Target board: Arduino Uno R4 WiFi (Renesas RA4M1 + ESP32-S3 radio via WiFiS3)

  5 rooms. Each room has:
    - 1 flame sensor (digital)   on D2..D6
    - 1 MQ-2 smoke sensor (analog) on A0..A4
    - 1 bulb on a relay channel  on D7..D11
  Plus:
    - 1 active buzzer on D12 (active-HIGH)

  Behavior:
    * Flame detected in room N -> immediately turn off relay N, sound buzzer,
      set fire lockout for that room, fire a one-shot notification after
      NOTIFICATION_DELAY_MS so transient triggers don't spam the backend.
    * Smoke (smoothed analog reading > SMOKE_THRESHOLD) -> sound buzzer,
      mark smoke alert, fire a one-shot notification per event.
      Light stays on unless TURN_OFF_LIGHT_ON_SMOKE = true.
    * The mobile app (future React Native) calls a small local JSON HTTP API
      to read status, toggle lights, and acknowledge alerts.
    * /reset-alert only clears state if every sensor currently reads safe.

  =========================  SAFETY  ====================================
  The relay module switches 220V AC mains. Mains wiring is LETHAL.
    - Have a qualified electrician do the AC side.
    - Use a properly enclosed, isolated relay assembly.
    - Never touch any AC wiring while powered.
    - Treat low-voltage and high-voltage grounds as ELECTRICALLY SEPARATE
      across the relay; only the DC control side shares ground with Arduino.
  Flame and MQ-2 modules are HOBBY-GRADE. Do NOT rely on this device as a
  certified life-safety fire alarm. Pair it with a real UL/EN-listed smoke
  alarm. MQ-2 sensors require a warm-up period before readings stabilize.
  ========================================================================
*/

#include <WiFiS3.h>

// ============================================================
// Credentials — edit these before flashing
// ============================================================
// To generate a strong API token: `openssl rand -hex 32`
#define SECRET_SSID      "YOUR_WIFI_SSID"
#define SECRET_PASS      "YOUR_WIFI_PASSWORD"

// Shared secret required by every state-changing POST endpoint.
// Anyone with this token can toggle relays and dismiss fire alerts on
// the LAN. Keep it long and random.
#define SECRET_API_TOKEN "change-me-to-a-long-random-string"

// ============================================================
// Configuration — tune these for your hardware and network
// ============================================================

// --- Network
const uint16_t SERVER_PORT                = 80;
const uint32_t WIFI_CONNECT_TIMEOUT_MS    = 10000;  // setup() gives up after this and lets loop() retry
const uint32_t WIFI_RECONNECT_INTERVAL_MS = 10000;  // backoff between reconnect attempts
const uint32_t HTTP_CLIENT_TIMEOUT_MS     = 2000;   // drop a slow/silent HTTP client after this

// --- Hardware polarity (flip these if your modules behave oppositely)
const bool RELAY_ACTIVE_LOW = true;   // most cheap 8-channel relay modules energize when IN pulled LOW
const bool FLAME_ACTIVE_LOW = true;   // most flame modules pull D0 LOW when flame is detected

// --- Detection tuning
const int      SMOKE_THRESHOLD       = 400;   // raw 0..1023; calibrate after warm-up
const uint8_t  SMOKE_SAMPLES         = 8;     // rolling-average window for MQ-2
const uint32_t FLAME_DEBOUNCE_MS     = 250;   // flame line must hold state this long
const uint32_t NOTIFICATION_DELAY_MS = 3000;  // delay before firing one-shot notification
const uint32_t SENSOR_WARMUP_MS      = 30000; // suppress smoke alerts during MQ-2 warm-up
const bool     TURN_OFF_LIGHT_ON_SMOKE = false;

// --- Pins (array index 0..4 == room 1..5)
const uint8_t FLAME_PINS[5] = {2, 3, 4, 5, 6};
const uint8_t SMOKE_PINS[5] = {A0, A1, A2, A3, A4};
const uint8_t RELAY_PINS[5] = {7, 8, 9, 10, 11};
const uint8_t BUZZER_PIN    = 12;
const uint8_t ROOM_COUNT    = 5;

// ============================================================
// State
// ============================================================

struct Room {
  bool     lightOn;
  bool     flameRaw;             // polarity-normalized current reading
  bool     flameDetected;        // post-debounce state
  uint32_t flameLastChangeMs;
  uint16_t smokeBuf[SMOKE_SAMPLES];
  uint8_t  smokeIdx;
  uint16_t smokeAvg;
  bool     smokeAlert;
  bool     fireLockout;
  bool     notifiedFire;
  bool     notifiedSmoke;
  uint32_t fireAlertSinceMs;
  uint32_t smokeAlertSinceMs;
};

Room     rooms[ROOM_COUNT];
WiFiServer server(SERVER_PORT);

bool     buzzerOn        = false;
bool     globalAlert     = false;
int8_t   lastAlertRoom   = -1;     // -1 = none, otherwise 0-based index
uint32_t bootMs          = 0;
uint32_t lastWifiAttempt = 0;
bool     serverStarted   = false;

// HTTP request state — handleHttpClient() is non-blocking so the safety loop
// (sensor reads + flame cut-off + buzzer) never waits on a slow or silent TCP
// client. We hold one in-flight client across loop iterations and consume a
// small byte budget each tick; a deadline trims any stalled connection.
WiFiClient httpClient;
String     httpRequestLine;
uint32_t   httpClientStartMs = 0;   // when the in-flight request began (for rollover-safe timeout)
bool       httpClientActive  = false;

// ============================================================
// Forward declarations
// ============================================================
void     maintainWifi(uint32_t now);
void     readSensors(uint32_t now);
void     updateAlerts(uint32_t now);
void     setLight(uint8_t roomIndex, bool on);
void     setBuzzer(bool on);
void     handleHttpClient();
void     sendJsonStatus(WiFiClient &client);
void     sendAlertNotification(uint8_t roomIndex, const char *reason);
bool     resetAlertsIfSafe();
void     sendResponse(WiFiClient &client, int code, const char *contentType, const String &body);
String   queryParam(const String &query, const String &key);
const char *statusText(int code);

// ============================================================
// setup() / loop()
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(100); // brief startup window for the USB CDC serial to attach

  Serial.println();
  Serial.println(F("[BOOT] SmartLightFireAlarm starting"));

  // Token sanity check — warn loudly if it's missing, short, or still the placeholder.
  if (strcmp(SECRET_API_TOKEN, "change-me-to-a-long-random-string") == 0 ||
      strlen(SECRET_API_TOKEN) < 16) {
    Serial.println(F("[WARN] SECRET_API_TOKEN is missing/short/default — anyone on the LAN"));
    Serial.println(F("[WARN] could dismiss fire alerts or flip relays. Edit arduino.ino."));
  }

  // Pin modes
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    pinMode(FLAME_PINS[i], INPUT);
    // Pre-load the output latch to the relay's INACTIVE level BEFORE switching
    // the pin to OUTPUT. Otherwise the pin transitions through the default LOW
    // state, which momentarily energizes an active-LOW relay at boot.
    digitalWrite(RELAY_PINS[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PINS[i], OUTPUT);
    rooms[i].lightOn           = false;
    rooms[i].flameRaw          = false;
    rooms[i].flameDetected     = false;
    rooms[i].flameLastChangeMs = 0;
    rooms[i].smokeIdx          = 0;
    rooms[i].smokeAvg          = 0;
    rooms[i].smokeAlert        = false;
    rooms[i].fireLockout       = false;
    rooms[i].notifiedFire      = false;
    rooms[i].notifiedSmoke     = false;
    rooms[i].fireAlertSinceMs  = 0;
    rooms[i].smokeAlertSinceMs = 0;
    for (uint8_t s = 0; s < SMOKE_SAMPLES; s++) rooms[i].smokeBuf[s] = 0;
  }
  // Same pre-load order for the buzzer (active-HIGH, so default LOW would already
  // be safe — applying the pattern uniformly so the convention is consistent).
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUZZER_PIN, OUTPUT);

  // Try WiFi with a bounded wait. loop() will keep retrying if this fails.
  Serial.print(F("[WIFI] connecting to "));
  Serial.println(SECRET_SSID);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    server.begin();
    serverStarted = true;
    Serial.print(F("[WIFI] connected ip="));
    Serial.print(WiFi.localIP());
    Serial.print(F(" rssi="));
    Serial.println(WiFi.RSSI());
    Serial.print(F("[HTTP] listening port="));
    Serial.println(SERVER_PORT);
  } else {
    Serial.println(F("[WIFI] not connected — will retry from loop()"));
  }

  lastWifiAttempt = millis();
  bootMs          = millis();
}

void loop() {
  uint32_t now = millis();
  maintainWifi(now);
  readSensors(now);
  updateAlerts(now);
  handleHttpClient();
}

// ============================================================
// WiFi maintenance (non-blocking outside the actual reconnect call)
// ============================================================
void maintainWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
      Serial.print(F("[WIFI] connected ip="));
      Serial.println(WiFi.localIP());
      Serial.print(F("[HTTP] listening port="));
      Serial.println(SERVER_PORT);
    }
    return;
  }

  if ((now - lastWifiAttempt) < WIFI_RECONNECT_INTERVAL_MS) return;
  lastWifiAttempt = now;
  Serial.println(F("[WIFI] reconnecting..."));
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  // WiFi.begin() on WiFiS3 can block for several seconds on the radio handshake.
  // Already-cut-off relays and the buzzer HOLD their last commanded state during
  // that window, but readSensors()/updateAlerts() do NOT run while WiFi.begin()
  // is in progress — so a fresh flame event that starts mid-reconnect won't
  // trigger its relay cut-off until the call returns. Accepted v1 trade-off:
  // (a) reconnects only happen when the network is already down, (b) a
  // certified UL/EN smoke alarm should be the primary safety device. For
  // stronger guarantees, drive sensor I/O from a hardware timer ISR or move
  // to a fully async WiFi stack.
}

// ============================================================
// Sensor sampling
// ============================================================
void readSensors(uint32_t now) {
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    // Flame (digital, polarity-normalized)
    int raw = digitalRead(FLAME_PINS[i]);
    bool active = FLAME_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
    if (active != rooms[i].flameRaw) {
      rooms[i].flameRaw          = active;
      rooms[i].flameLastChangeMs = now;
    }

    // Smoke (analog, smoothed)
    uint16_t s = analogRead(SMOKE_PINS[i]);
    rooms[i].smokeBuf[rooms[i].smokeIdx] = s;
    rooms[i].smokeIdx = (rooms[i].smokeIdx + 1) % SMOKE_SAMPLES;
    uint32_t sum = 0;
    for (uint8_t k = 0; k < SMOKE_SAMPLES; k++) sum += rooms[i].smokeBuf[k];
    rooms[i].smokeAvg = sum / SMOKE_SAMPLES;
  }
}

// ============================================================
// Alert state machine
// ============================================================
void updateAlerts(uint32_t now) {
  bool anyAlert = false;
  bool warmedUp = (now - bootMs) >= SENSOR_WARMUP_MS;

  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    Room &r = rooms[i];

    // --- Flame: debounce
    bool prev = r.flameDetected;
    if (r.flameRaw != r.flameDetected && (now - r.flameLastChangeMs) >= FLAME_DEBOUNCE_MS) {
      r.flameDetected = r.flameRaw;
    }

    // Rising edge: fire just confirmed -> immediate cut-off + lockout
    if (r.flameDetected && !prev) {
      Serial.print(F("[FIRE] room="));
      Serial.println(i + 1);
      setLight(i, false);
      r.fireLockout      = true;
      r.fireAlertSinceMs = now;
      r.notifiedFire     = false;
      lastAlertRoom      = i;
    }
    // Falling edge: sensor cleared (lockout persists until /reset-alert)
    if (!r.flameDetected && prev) {
      Serial.print(F("[FIRE] room="));
      Serial.print(i + 1);
      Serial.println(F(" cleared (lockout still held until reset)"));
    }

    // Fire notification: one-shot, after stable delay
    if (r.flameDetected && !r.notifiedFire &&
        (now - r.fireAlertSinceMs) >= NOTIFICATION_DELAY_MS) {
      sendAlertNotification(i, "fire");
      r.notifiedFire = true;
    }

    // --- Smoke: smoothed threshold
    bool smokeOver = warmedUp && (r.smokeAvg > SMOKE_THRESHOLD);
    if (smokeOver && !r.smokeAlert) {
      r.smokeAlert        = true;
      r.smokeAlertSinceMs = now;
      r.notifiedSmoke     = false;
      lastAlertRoom       = i;
      Serial.print(F("[SMOKE] room="));
      Serial.print(i + 1);
      Serial.print(F(" value="));
      Serial.println(r.smokeAvg);
      if (TURN_OFF_LIGHT_ON_SMOKE) setLight(i, false);
    } else if (!smokeOver && r.smokeAlert) {
      r.smokeAlert    = false;
      r.notifiedSmoke = false; // allow re-notify on next crossing
      Serial.print(F("[SMOKE] room="));
      Serial.print(i + 1);
      Serial.println(F(" cleared"));
    }
    if (r.smokeAlert && !r.notifiedSmoke &&
        (now - r.smokeAlertSinceMs) >= NOTIFICATION_DELAY_MS) {
      sendAlertNotification(i, "smoke");
      r.notifiedSmoke = true;
    }

    if (r.flameDetected || r.smokeAlert) anyAlert = true;
  }

  globalAlert = anyAlert;
  setBuzzer(anyAlert);
}

// ============================================================
// Actuators
// ============================================================
void setLight(uint8_t i, bool on) {
  if (i >= ROOM_COUNT) return;
  rooms[i].lightOn = on;
  bool pinHigh = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PINS[i], pinHigh ? HIGH : LOW);
}

void setBuzzer(bool on) {
  if (on == buzzerOn) return; // idempotent — avoid extra writes
  buzzerOn = on;
  digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
}

// ============================================================
// Reset / acknowledge
// ============================================================
bool resetAlertsIfSafe() {
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (rooms[i].flameDetected || rooms[i].flameRaw || rooms[i].smokeAlert) {
      return false;
    }
  }
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    rooms[i].fireLockout   = false;
    rooms[i].notifiedFire  = false;
    rooms[i].notifiedSmoke = false;
  }
  globalAlert   = false;
  lastAlertRoom = -1;
  setBuzzer(false);
  Serial.println(F("[RESET] alerts cleared"));
  return true;
}

// ============================================================
// Notification dispatch — v1 stub.
// Future React Native backend integration goes inside this function.
// Keep any HTTP call non-blocking (short timeout, fire-and-forget).
// ============================================================
void sendAlertNotification(uint8_t roomIndex, const char *reason) {
  Serial.print(F("[ALERT] room="));
  Serial.print(roomIndex + 1);
  Serial.print(F(" reason="));
  Serial.print(reason);
  Serial.print(F(" smoke="));
  Serial.print(rooms[roomIndex].smokeAvg);
  Serial.print(F(" ts="));
  Serial.println(millis());

  // TODO(rn-backend): plug your push/SMS/MQTT integration here. Sketch:
  //
  //   WiFiClient http;
  //   if (http.connect("your-backend.example.com", 80)) {
  //     String body = "{\"device\":\"uno-r4\",\"room\":";
  //     body += (roomIndex + 1);
  //     body += ",\"reason\":\"";
  //     body += reason;
  //     body += "\"}";
  //     http.print(F("POST /alerts HTTP/1.1\r\nHost: your-backend.example.com\r\n"));
  //     http.print(F("Content-Type: application/json\r\nContent-Length: "));
  //     http.print(body.length());
  //     http.print(F("\r\nConnection: close\r\n\r\n"));
  //     http.print(body);
  //     http.stop();
  //   }
  //
  // For FCM or Twilio: route through your own backend so the API secrets
  // never live on the Arduino. The backend then fans out to FCM/SMS/MQTT.
}

// ============================================================
// HTTP server
// ============================================================
void handleHttpClient() {
  if (!serverStarted) return;

  uint32_t now = millis();

  // Accept a new client only if we're not still servicing one. server.available()
  // is cheap and non-blocking on WiFiS3.
  if (!httpClientActive) {
    WiFiClient c = server.available();
    if (!c) return;
    httpClient        = c;
    httpRequestLine   = "";
    httpRequestLine.reserve(128);
    httpClientStartMs = now;          // start the timeout window (rollover-safe via subtraction below)
    httpClientActive  = true;
  }

  // Watchdog: drop the client if it goes silent or stalls past the timeout.
  // We check this BEFORE reading so a dead client is reclaimed within one tick.
  // Unsigned subtraction is rollover-safe — wrapping past UINT32_MAX still
  // yields the correct elapsed time as long as it's well under ~49 days.
  if ((now - httpClientStartMs) >= HTTP_CLIENT_TIMEOUT_MS) {
    httpClient.stop();
    httpClientActive = false;
    return;
  }
  if (!httpClient.connected() && !httpClient.available()) {
    httpClient.stop();
    httpClientActive = false;
    return;
  }

  // Read a small budget of bytes this tick. We yield back to loop() between
  // ticks so readSensors() / updateAlerts() keep running every iteration.
  // This is the safety-critical change: a slow or malicious client can no
  // longer delay flame detection or the relay cut-off.
  const uint8_t MAX_BYTES_PER_TICK = 64;
  uint8_t budget = MAX_BYTES_PER_TICK;
  bool lineComplete = false;
  while (budget-- > 0 && httpClient.available()) {
    char c = httpClient.read();
    if (c == '\n') { lineComplete = true; break; }
    if (c != '\r' && httpRequestLine.length() < 256) httpRequestLine += c;
  }
  if (!lineComplete) return; // come back next loop iteration

  // Drain any remaining input non-blocking (headers/body we don't parse).
  // Bounded so we don't loop forever on a chatty client.
  uint8_t drainBudget = 128;
  while (drainBudget-- > 0 && httpClient.available()) httpClient.read();

  // ----- Route the completed request line -----
  String &line = httpRequestLine;
  int s1 = line.indexOf(' ');
  int s2 = line.indexOf(' ', s1 + 1);
  if (s1 < 0 || s2 < 0) {
    sendResponse(httpClient, 400, "application/json", "{\"error\":\"bad request\"}");
  } else {
    String method = line.substring(0, s1);
    String url    = line.substring(s1 + 1, s2);
    String path   = url;
    String query  = "";
    int q = url.indexOf('?');
    if (q >= 0) {
      path  = url.substring(0, q);
      query = url.substring(q + 1);
    }

    if (method == "OPTIONS") {
      // CORS preflight passes without a token so legit browser callers can probe.
      sendResponse(httpClient, 204, "text/plain", "");
    } else if (method == "POST" && queryParam(query, "token") != SECRET_API_TOKEN) {
      // CSRF defense: every state-changing POST requires the shared secret.
      // CORS being permissive is fine — the token is what actually gates writes.
      sendResponse(httpClient, 401, "application/json", "{\"error\":\"unauthorized\"}");
    } else if (method == "GET" && path == "/health") {
      sendResponse(httpClient, 200, "application/json", "{\"ok\":true}");
    } else if (method == "GET" && path == "/status") {
      sendJsonStatus(httpClient);
    } else if (method == "POST" && path == "/light") {
      String roomStr  = queryParam(query, "room");
      String stateStr = queryParam(query, "state");
      int roomNum = roomStr.toInt();
      if (roomNum < 1 || roomNum > ROOM_COUNT ||
          (stateStr != "on" && stateStr != "off")) {
        sendResponse(httpClient, 400, "application/json",
                     "{\"error\":\"bad params\",\"hint\":\"room=1..5 & state=on|off\"}");
      } else {
        uint8_t idx = roomNum - 1;
        bool want   = (stateStr == "on");
        if (want && rooms[idx].fireLockout) {
          String body = "{\"error\":\"room in fire lockout\",\"room\":";
          body += roomNum;
          body += "}";
          sendResponse(httpClient, 423, "application/json", body);
        } else {
          setLight(idx, want);
          String body = "{\"ok\":true,\"room\":";
          body += roomNum;
          body += ",\"light\":";
          body += (want ? "true" : "false");
          body += "}";
          sendResponse(httpClient, 200, "application/json", body);
        }
      }
    } else if (method == "POST" && path == "/all-lights") {
      String stateStr = queryParam(query, "state");
      if (stateStr != "off") {
        sendResponse(httpClient, 400, "application/json",
                     "{\"error\":\"only state=off supported\"}");
      } else {
        for (uint8_t i = 0; i < ROOM_COUNT; i++) setLight(i, false);
        sendResponse(httpClient, 200, "application/json",
                     "{\"ok\":true,\"allOff\":true}");
      }
    } else if (method == "POST" && path == "/reset-alert") {
      if (resetAlertsIfSafe()) {
        sendResponse(httpClient, 200, "application/json", "{\"reset\":true}");
      } else {
        sendResponse(httpClient, 409, "application/json",
                     "{\"reset\":false,\"reason\":\"sensors still alerting\"}");
      }
    } else {
      sendResponse(httpClient, 404, "application/json", "{\"error\":\"not found\"}");
    }
  }

  httpClient.stop();
  httpClientActive = false;
}

void sendJsonStatus(WiFiClient &client) {
  String body;
  body.reserve(640);
  body += "{\"wifi\":{\"connected\":";
  body += (WiFi.status() == WL_CONNECTED ? "true" : "false");
  body += ",\"ip\":\"";
  IPAddress ip = WiFi.localIP();
  body += ip[0]; body += "."; body += ip[1]; body += "."; body += ip[2]; body += "."; body += ip[3];
  body += "\",\"rssi\":";
  body += (int)WiFi.RSSI();
  body += "},\"uptimeMs\":";
  body += (uint32_t)(millis() - bootMs);
  body += ",\"buzzer\":";
  body += (buzzerOn ? "true" : "false");
  body += ",\"alertActive\":";
  body += (globalAlert ? "true" : "false");
  body += ",\"lastAlertRoom\":";
  // null when no alert has fired since boot/reset — UIs render this as "none"
  // instead of a misleading "Room 0".
  if (lastAlertRoom < 0) body += "null";
  else                   body += (lastAlertRoom + 1);
  body += ",\"rooms\":[";
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i > 0) body += ",";
    body += "{\"room\":";
    body += (i + 1);
    body += ",\"light\":";
    body += (rooms[i].lightOn ? "true" : "false");
    body += ",\"flame\":";
    body += (rooms[i].flameDetected ? "true" : "false");
    body += ",\"smoke\":";
    body += rooms[i].smokeAvg;
    body += ",\"smokeAlert\":";
    body += (rooms[i].smokeAlert ? "true" : "false");
    body += ",\"lockout\":";
    body += (rooms[i].fireLockout ? "true" : "false");
    body += "}";
  }
  body += "]}";
  sendResponse(client, 200, "application/json", body);
}

void sendResponse(WiFiClient &client, int code, const char *contentType, const String &body) {
  client.print(F("HTTP/1.1 "));
  client.print(code);
  client.print(' ');
  client.println(statusText(code));
  client.print(F("Content-Type: "));
  client.println(contentType);
  client.print(F("Content-Length: "));
  client.println(body.length());
  client.println(F("Access-Control-Allow-Origin: *"));
  client.println(F("Access-Control-Allow-Methods: GET, POST, OPTIONS"));
  client.println(F("Access-Control-Allow-Headers: Content-Type"));
  client.println(F("Connection: close"));
  client.println();
  client.print(body);
}

String queryParam(const String &query, const String &key) {
  int start = 0;
  int len = query.length();
  while (start < len) {
    int amp = query.indexOf('&', start);
    if (amp < 0) amp = len;
    int eq = query.indexOf('=', start);
    if (eq > 0 && eq < amp && query.substring(start, eq) == key) {
      return query.substring(eq + 1, amp);
    }
    start = amp + 1;
  }
  return "";
}

const char *statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 423: return "Locked";
    case 500: return "Internal Server Error";
    default:  return "OK";
  }
}
