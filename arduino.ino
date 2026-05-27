/*
  arduino.ino
  IoT-Based Smart Lighting Automation and Fire Alarm System
  Target board: Arduino Uno R4 WiFi (Renesas RA4M1 + ESP32-S3 radio via WiFiS3)

  5 rooms. Each room has:
    - 1 flame sensor (digital)   on D2..D6
    - 1 MQ-2 smoke sensor (analog) on A0..A4
    - 1 bulb on a relay channel  on D7..D11
  Plus:
    - 1 active buzzer on D12 (level configurable via BUZZER_ACTIVE_LEVEL)

  Behavior:
    * Flame OR smoke detected in any room -> cut every light, latch every
      room's fireLockout, sound the buzzer, fire a one-shot notification
      after NOTIFICATION_DELAY_MS. Smoke and fire share the same safety
      path (see tripAllLightsForHazard()).
    * The buzzer auto-silences after BUZZER_ALARM_MS (default 2 min) even
      if the hazard persists. Lights stay OFF (lockout) until /reset-alert.
      Hazard clears then returns -> buzzer re-arms and sounds again.
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

// =============================================================
// BUZZER POLARITY — flip ONE constant to match your module
// =============================================================
// Set this to true  if applying +5V (HIGH) to D12 makes the buzzer sound
//   (typical for bare 2-pin piezo buzzers wired + -> D12, - -> GND).
// Set this to false if applying  0V (LOW)  to D12 makes the buzzer sound
//   (typical for 3-pin "active buzzer" modules with onboard transistor,
//    and for 2-pin buzzers wired + -> 5V, - -> D12).
//
// SYMPTOM -> ACTION:
//   * Buzzer beeps continuously at boot with no fire/smoke present
//     -> flip BUZZER_ACTIVE_HIGH to the OPPOSITE value, re-flash.
//   * Buzzer stays SILENT during a confirmed flame ([FIRE] in Serial)
//     -> flip BUZZER_ACTIVE_HIGH to the OPPOSITE value, re-flash.
//
// If both directions still misbehave, the buzzer is wired directly to
// 5V/GND (not D12) — run BUZZER_FORCE_PIN_LOW_TEST below to prove it.
//
// Other diagnosis hints (read [TELEM] lines on Serial @ 115200):
//   * [TELEM] shows flame=YES on a room with no flame -> a per-room safe
//     baseline is wrong; check the [CAL] line, wiring/pot for that pin,
//     or temporarily set FLAME_SENSOR_ENABLED[i]=false.
//   * [TELEM] shows mq2 baseline > threshold after warm -> raise
//     SMOKE_THRESHOLDS[i] or tune SMOKE_DELTA_THRESHOLD.
const bool          BUZZER_ACTIVE_HIGH      = false;             // <-- one-knob polarity flip
const int           BUZZER_ON_LEVEL         = BUZZER_ACTIVE_HIGH ? HIGH : LOW;
const int           BUZZER_OFF_LEVEL        = BUZZER_ACTIVE_HIGH ? LOW  : HIGH;
// Legacy aliases — used throughout the rest of the sketch (setBuzzer(),
// proof tests, telemetry). Defined in terms of the derived levels so
// retuning polarity only requires editing BUZZER_ACTIVE_HIGH above.
const int           BUZZER_ACTIVE_LEVEL     = BUZZER_ON_LEVEL;
const int           BUZZER_INACTIVE_LEVEL   = BUZZER_OFF_LEVEL;
const bool          RELAY_ACTIVE_LOW        = true;  // most 8-ch relay boards energize when IN pulled LOW

// --- Buzzer hardware diagnostics (set to 'true' temporarily to isolate
// firmware-vs-wiring problems; ALWAYS reset to 'false' before regular use).
//
// BUZZER_FORCE_DISABLED:
//   When true, every setBuzzer(true) is rewritten to write the inactive
//   level. Use this to PROVE the buzzer is no longer being driven by
//   firmware. If it still beeps with this flag on, the buzzer is not
//   actually controlled by D12 (wrong pin or wired directly to 5V/GND).
//
// BUZZER_DIAGNOSTIC_MODE:
//   When true, all sensor reads, alert evaluation, WiFi maintenance, and
//   HTTP serving are skipped. The loop only blinks D12 between INACTIVE
//   and ACTIVE on a 1 s ON / 3 s OFF cadence. Use this to PROVE D12
//   actually controls the buzzer. If the buzzer does not follow the
//   blink, the buzzer is wired wrong (different pin, no signal
//   connection, or directly across rails).
const bool          BUZZER_FORCE_DISABLED        = false;
const bool          BUZZER_DIAGNOSTIC_MODE       = false;
const unsigned long BUZZER_DIAGNOSTIC_ON_MS      = 1000;
const unsigned long BUZZER_DIAGNOSTIC_OFF_MS     = 3000;

// =============================================================
// BUZZER HARDWARE PROOF CONFIG
// =============================================================
// Use these tests to prove whether the physical buzzer is controlled
// by D12 and whether the module is active HIGH or active LOW.
// Set EXACTLY ONE to true, upload, observe the buzzer, then set all
// back to false for normal operation. Proof mode overrides
// COMMISSIONING_MODE, BUZZER_DIAGNOSTIC_MODE, sensors, WiFi, HTTP,
// telemetry, and relays — only D12 is touched.
//
// Test result matrix:
//   LOW beeps, HIGH silent          -> active-LOW buzzer module.
//                                      Set BUZZER_ACTIVE_LEVEL=LOW,
//                                      BUZZER_INACTIVE_LEVEL=HIGH.
//   HIGH beeps, LOW silent          -> active-HIGH buzzer module.
//                                      Set BUZZER_ACTIVE_LEVEL=HIGH,
//                                      BUZZER_INACTIVE_LEVEL=LOW.
//   LOW beeps AND HIGH beeps        -> buzzer wired directly to power,
//                                      wired to the wrong pin, or a
//                                      3-pin module powered without
//                                      SIG actually used. Firmware
//                                      cannot silence it.
//   LOW silent AND HIGH silent      -> buzzer not connected, broken,
//                                      or on a different pin entirely.
//   FLOAT still beeps               -> buzzer is not controlled by D12.
//                                      Check wiring/module type.
const bool          BUZZER_FORCE_PIN_LOW_TEST    = false;
const bool          BUZZER_FORCE_PIN_HIGH_TEST   = false;
const bool          BUZZER_FLOAT_PIN_TEST        = false;

// =============================================================
// DEBUG / COMMISSIONING CONFIG
// =============================================================
// ***** MUST BE false FOR REAL FIRE-SAFETY OPERATION *****
// COMMISSIONING_MODE=true means: read sensors, compute would-be alerts,
// print everything to Serial, but do NOT latch alerts, do NOT lock out
// lights, and do NOT energise the buzzer from sensor decisions. Use this
// while wiring/tuning sensors so the buzzer is not constantly screaming.
// Telemetry will report `commissioning=YES wouldAlert=... actualAlert=no
// buzzerCmd=OFF reason=commissioning_suppressed` so you can iterate on
// thresholds and polarity without ever firing the alarm.
const bool COMMISSIONING_MODE = false;

// If a sensor's auto-calibrated safe level is wrong (e.g. rooms 2/3 in
// the field telemetry learned safe=H but then immediately read raw=L),
// set this true to bypass auto-calibration entirely and use the manual
// MANUAL_FLAME_DETECTED_LEVELS[] entries below. Useful when calibration
// keeps learning the wrong baseline because a sensor is flipping state
// during the 5 s calibration window.
const bool USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING = false;

// Optional stronger anti-flicker for noisy flame sensors. After the
// FLAME_DEBOUNCE_MS window, additionally require this many consecutive
// matching reads before promoting flameRaw -> flameDetected. 1 = behavior
// before this constant existed (no extra confirmation). 3 = require three
// back-to-back agreements; real fires hold for many seconds so the only
// thing this filters out is single noisy edges from a marginal sensor.
const uint8_t       FLAME_CONFIRMATION_SAMPLES   = 3;

// --- Flame sensor interpretation (per-room, auto-calibrated)
// Auto-calibration samples each flame pin during the first FLAME_CALIBRATION_MS
// after boot and learns each room's safe (no-flame) baseline. Keep ALL flame
// sensors away from flame during that window. After the window the "detected"
// level is the opposite of the learned safe level. This handles mixed-polarity
// modules and pins that idle LOW because they're disconnected/floating.
//
// Set AUTO_CALIBRATE_FLAME_SAFE_LEVEL=false to skip learning and use
// MANUAL_FLAME_DETECTED_LEVELS[] verbatim — useful only if calibration is
// unreliable for a given board.
const bool          AUTO_CALIBRATE_FLAME_SAFE_LEVEL = true;
const unsigned long FLAME_CALIBRATION_MS            = 5000;

// Per-room enable. During commissioning, set ONE entry to true at a time so
// only that sensor can drive `wouldAlert` — this is how you prove which
// physical channel is noisy / miswired without the others piling on. For
// real operation, set every entry to true. A `false` entry SUPPRESSES that
// room's fire alarm entirely; readSensors() forces flameRaw=false and
// updateAlerts() clears any lingering lockout.
// Length is hard-coded to 5 to match the rest of the per-room arrays; if you
// change ROOM_COUNT, update the initializer list below as well.
const bool          FLAME_SENSOR_ENABLED[5] = { true, true, true, true, true };

// Used when AUTO_CALIBRATE_FLAME_SAFE_LEVEL == false OR when
// USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING == true. Specifies the
// digital level the module drives DO to when it SEES flame.
//
// Most KY-026 / YL-38 / generic flame modules use an LM393 comparator that
// pulls DO LOW when flame is detected (active-LOW). If your modules behave
// the opposite way (DO=HIGH on flame), flip these to HIGH per room.
// Telemetry's `raw=` field shows the current digital read of each pin, so
// you can compare against the at-rest behavior of each module and adjust.
//
//   Active-LOW modules (typical):  { LOW,  LOW,  LOW,  LOW,  LOW  }
//   Active-HIGH modules:           { HIGH, HIGH, HIGH, HIGH, HIGH }
//
// The HIGH default below is intentional for first-boot commissioning: it
// lets you observe `wouldAlert=YES` if your modules turn out to be active-
// LOW (because raw=H at rest would be interpreted as flame), and flip to
// LOW once you've confirmed the polarity from telemetry. COMMISSIONING_MODE
// keeps the buzzer silent while you decide. Length must match ROOM_COUNT (5).
const int           MANUAL_FLAME_DETECTED_LEVELS[5] = { LOW, LOW, LOW, LOW, LOW };

// --- Detection tuning
// Per-room smoke sensor enable. Same idea as FLAME_SENSOR_ENABLED — turn on
// one channel at a time during commissioning to find a misbehaving MQ-2
// without the others piling on. Disabled rooms are still sampled (so the
// raw value shows up in telemetry) but never trigger a smoke alert.
const bool          SMOKE_SENSOR_ENABLED[5] = { true, true, true, true, true };

// Per-room absolute smoke threshold (raw 0..1023). Used when
// USE_SMOKE_BASELINE_THRESHOLD is false, OR before baseline capture
// completes. Room 1's MQ-2 in this build idles around 800-900 in clean air,
// so its absolute threshold is raised; the others sit near 100-200 and stay
// at 500 until you have field data showing otherwise. Length must match
// ROOM_COUNT (5).
const int           SMOKE_THRESHOLDS[5]     = { 950, 500, 500, 500, 500 };

// Baseline+delta detection — preferred for MQ-2 because absolute clean-air
// values drift wildly between modules and with humidity. When enabled,
// readSensors() averages each sensor's reading across the warm-up window
// to learn its clean-air baseline, and updateAlerts() fires when the
// current value exceeds baseline + SMOKE_DELTA_THRESHOLD. Falls back to
// the absolute SMOKE_THRESHOLDS[] table until baseline capture finishes.
const bool          USE_SMOKE_BASELINE_THRESHOLD = true;
const int           SMOKE_DELTA_THRESHOLD        = 250;

const uint8_t       SMOKE_SAMPLES           = 8;     // rolling-average window for MQ-2
const uint32_t      FLAME_DEBOUNCE_MS       = 250;   // flame line must hold state this long
const uint32_t      NOTIFICATION_DELAY_MS   = 3000;  // delay before firing one-shot notification
const unsigned long SMOKE_STARTUP_IGNORE_MS = 30000; // suppress smoke alerts during MQ-2 warm-up

// Maximum time the buzzer is allowed to sound for a single hazard event
// before auto-silencing. Lights stay OFF (lockout) until /reset-alert; only
// the buzzer is gated by this timeout. When the hazard CLEARS and a NEW
// hazard arises, the buzzer is re-armed and sounds again for up to this
// many milliseconds. Set to 0 to disable auto-silence (legacy behavior).
const unsigned long BUZZER_ALARM_MS         = 120000UL; // 2 minutes

// --- Pins (array index 0..4 == room 1..5)
const uint8_t FLAME_PINS[5] = {2, 3, 4, 5, 6};
const uint8_t SMOKE_PINS[5] = {A0, A1, A2, A3, A4};
const uint8_t RELAY_PINS[5] = {7, 8, 9, 10, 11};
const uint8_t BUZZER_PIN    = 12;
const uint8_t ROOM_COUNT    = 5;

// Compile-time guard: if anyone bumps ROOM_COUNT, these asserts loudly remind
// them to extend the per-room config arrays declared above.
static_assert(sizeof(FLAME_PINS)                   / sizeof(FLAME_PINS[0])                   == ROOM_COUNT, "FLAME_PINS length must match ROOM_COUNT");
static_assert(sizeof(SMOKE_PINS)                   / sizeof(SMOKE_PINS[0])                   == ROOM_COUNT, "SMOKE_PINS length must match ROOM_COUNT");
static_assert(sizeof(RELAY_PINS)                   / sizeof(RELAY_PINS[0])                   == ROOM_COUNT, "RELAY_PINS length must match ROOM_COUNT");
static_assert(sizeof(FLAME_SENSOR_ENABLED)         / sizeof(FLAME_SENSOR_ENABLED[0])         == ROOM_COUNT, "FLAME_SENSOR_ENABLED length must match ROOM_COUNT");
static_assert(sizeof(MANUAL_FLAME_DETECTED_LEVELS) / sizeof(MANUAL_FLAME_DETECTED_LEVELS[0]) == ROOM_COUNT, "MANUAL_FLAME_DETECTED_LEVELS length must match ROOM_COUNT");
static_assert(sizeof(SMOKE_SENSOR_ENABLED)         / sizeof(SMOKE_SENSOR_ENABLED[0])         == ROOM_COUNT, "SMOKE_SENSOR_ENABLED length must match ROOM_COUNT");
static_assert(sizeof(SMOKE_THRESHOLDS)             / sizeof(SMOKE_THRESHOLDS[0])             == ROOM_COUNT, "SMOKE_THRESHOLDS length must match ROOM_COUNT");

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
bool     globalAlert     = false;   // ACTUAL alert (drives buzzer in safety mode)
bool     wouldAlert      = false;   // what the buzzer WOULD be doing if commissioning were off
int8_t   lastAlertRoom   = -1;      // -1 = none, otherwise 0-based index
uint32_t bootMs          = 0;
uint32_t lastWifiAttempt = 0;
bool     serverStarted   = false;

// Buzzer auto-silence state machine. The buzzer is driven by the CURRENT
// sensor reading (currentHazard), NOT the latched fireLockout, so a stuck
// latch can no longer hold the buzzer on forever. alarmStartedAt is the
// millis() of the rising edge of the current hazard; buzzerTimedOut latches
// true once the elapsed time exceeds BUZZER_ALARM_MS; prevHazardActive
// tracks the previous loop's hazard state for edge detection.
uint32_t alarmStartedAt   = 0;
bool     buzzerTimedOut   = false;
bool     prevHazardActive = false;
uint32_t buzzerOnUntilMs  = 0;      // 0 when silent; otherwise millis() when timeout will fire

// Last level actually written to D12 (HIGH or LOW). Updated only when
// digitalWrite() is invoked. Reported in [TELEM] so a firmware-vs-wiring
// mismatch is visible from Serial alone — if `buzzerCmd=OFF` but
// `buzzerPinWrite=HIGH`, the alert path and the GPIO state disagree.
int      lastBuzzerPinLevel = BUZZER_INACTIVE_LEVEL;

// Per-room stuck / noise tracking for the flame inputs. Populated in
// readSensors() after calibration completes. The warnings each fire at
// most once per boot so the Serial log stays grep-friendly.
uint32_t flameLastEdgeMs[ROOM_COUNT]          = {0, 0, 0, 0, 0};
uint16_t flameEdgesInWindow[ROOM_COUNT]       = {0, 0, 0, 0, 0};
uint32_t flameNoiseWindowStart[ROOM_COUNT]    = {0, 0, 0, 0, 0};
uint8_t  flameConfirmStreak[ROOM_COUNT]       = {0, 0, 0, 0, 0};
bool     flameNoisyWarned[ROOM_COUNT]         = {false, false, false, false, false};
bool     flameStuckPostCalWarned[ROOM_COUNT]  = {false, false, false, false, false};
uint32_t flameDetectedSinceMs[ROOM_COUNT]     = {0, 0, 0, 0, 0};

// Cached raw level captured once in readSensors() and reused in
// logTelemetry() + sendJsonStatus() so all three reporters agree on the
// same physical sample within one loop iteration. Without this cache,
// rapid sensor noise can make telemetry `raw=` disagree with the
// `flameRaw=` / `flame=` values from the same loop iteration.
int      flameRawLevel[ROOM_COUNT]            = {0, 0, 0, 0, 0};

// Flame calibration / interpretation runtime state. Populated in setup() and
// finalized inside readSensors() once FLAME_CALIBRATION_MS has elapsed.
int      flameSafeLevels[ROOM_COUNT]       = {0, 0, 0, 0, 0};
int      flameDetectedLevels[ROOM_COUNT]   = {0, 0, 0, 0, 0};
uint16_t flameHighCount[ROOM_COUNT]        = {0, 0, 0, 0, 0};
uint16_t flameLowCount[ROOM_COUNT]         = {0, 0, 0, 0, 0};
bool     flameCalibrationComplete          = false;
uint32_t flameCalibrationStartedAt         = 0;
bool     flameStuckWarningShown[ROOM_COUNT] = {false, false, false, false, false};

// Why is the buzzer on right now? Surfaced in [TELEM] + /status so the user
// can immediately see which subsystem is driving the alarm without digging.
// Possible values:
//   "off"                       — no sensor alert, no latch
//   "commissioning_suppressed"  — wouldAlert=true but COMMISSIONING_MODE muted it
//   "flame"                     — at least one room has confirmed flame
//   "smoke"                     — at least one room is above smoke threshold/delta
//   "flame+smoke"               — both above are true
//   "latched"                   — sensors clear but a fireLockout is still held
//   "timed_out"                 — hazard still present but BUZZER_ALARM_MS expired
//   "force_disabled"            — BUZZER_FORCE_DISABLED short-circuited an alert
//   "diagnostic"                — BUZZER_DIAGNOSTIC_MODE owns the pin
const char *buzzerReason = "off";

// Smoke baseline tracking (analogous to flameSafeLevels[]). During the
// SMOKE_STARTUP_IGNORE_MS warm-up window each enabled sensor's reading is
// accumulated; when warm-up finishes the average becomes the per-room
// baseline. Used only when USE_SMOKE_BASELINE_THRESHOLD is true.
int      smokeBaseline[ROOM_COUNT]        = {0, 0, 0, 0, 0};
uint32_t smokeBaselineSum[ROOM_COUNT]     = {0, 0, 0, 0, 0};
uint16_t smokeBaselineSamples[ROOM_COUNT] = {0, 0, 0, 0, 0};
bool     smokeBaselineReady               = false;

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
void     setBuzzer(bool on, const char *reason);
bool     anyFireDetected();
bool     anySmokeDetected();
void     tripAllLightsForHazard();
bool     buzzerProofActive(const char *&selectedTest);
void     applyBuzzerProof(const char *selectedTest, bool firstCall);
void     handleHttpClient();
void     logTelemetry(uint32_t now);
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
  // *** FIRST THING WE DO ***
  // Park D12 at the buzzer's OFF level the instant we have CPU time —
  // before Serial.begin() and its 100 ms USB CDC delay. This minimizes
  // the floating-input window at boot from ~100 ms down to microseconds
  // so the buzzer cannot chirp during board reset. Polarity is governed
  // entirely by BUZZER_ACTIVE_HIGH at the top of this file.
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL); // enables matching pullup while still INPUT
  pinMode(BUZZER_PIN, OUTPUT);                // start actively driving the pin
  digitalWrite(BUZZER_PIN, BUZZER_OFF_LEVEL); // re-assert hard drive after pinMode
  lastBuzzerPinLevel = BUZZER_OFF_LEVEL;
  buzzerOn = false;

  Serial.begin(115200);
  delay(100); // brief startup window for the USB CDC serial to attach

  Serial.println();
  Serial.println(F("[BOOT] SmartLightFireAlarm starting"));

  // BUZZER HARDWARE PROOF — when any BUZZER_*_TEST constant is true,
  // bypass the rest of setup (relays, sensors, calibration, WiFi) and
  // only drive D12. The loop will keep re-asserting the chosen state.
  // This isolates "is the buzzer controlled by D12?" from every other
  // subsystem on the board.
  {
    const char *proofMode = nullptr;
    if (buzzerProofActive(proofMode)) {
      applyBuzzerProof(proofMode, /*firstCall=*/true);
      bootMs = millis();
      return;
    }
  }

  // Token sanity check — warn loudly if it's missing, short, or still the placeholder.
  if (strcmp(SECRET_API_TOKEN, "change-me-to-a-long-random-string") == 0 ||
      strlen(SECRET_API_TOKEN) < 16) {
    Serial.println(F("[WARN] SECRET_API_TOKEN is missing/short/default — anyone on the LAN"));
    Serial.println(F("[WARN] could dismiss fire alerts or flip relays. Edit arduino.ino."));
  }

  // Print active configuration so the Serial Monitor shows which polarity flags
  // and thresholds are in effect. If the buzzer is misbehaving, the user reads
  // these alongside the [TELEM] lines below to decide which constant to flip.
  Serial.print(F("[CFG] COMMISSIONING_MODE="));
  if (COMMISSIONING_MODE) {
    Serial.println(F("YES (sensor alerts will NOT drive buzzer/lockout)"));
  } else {
    Serial.println(F("no (production safety behavior)"));
  }
  Serial.print(F("[CFG] USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING="));
  Serial.println(USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING ? F("YES") : F("no"));
  Serial.print(F("[CFG] BUZZER_ACTIVE_HIGH="));
  Serial.print(BUZZER_ACTIVE_HIGH ? F("YES") : F("no"));
  Serial.print(F("  ON_LEVEL="));
  Serial.print(BUZZER_ON_LEVEL == HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F("  OFF_LEVEL="));
  Serial.println(BUZZER_OFF_LEVEL == HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F("[CFG] BUZZER_ALARM_MS="));
  Serial.print(BUZZER_ALARM_MS);
  if (BUZZER_ALARM_MS == 0) {
    Serial.println(F(" (auto-silence disabled)"));
  } else {
    Serial.println();
  }
  Serial.print(F("[CFG] BUZZER_FORCE_DISABLED="));
  Serial.println(BUZZER_FORCE_DISABLED ? F("YES (alerts suppressed)") : F("no"));
  Serial.print(F("[CFG] BUZZER_DIAGNOSTIC_MODE="));
  Serial.println(BUZZER_DIAGNOSTIC_MODE ? F("YES (D12 blink pattern only)") : F("no"));
  Serial.print(F("[CFG] FLAME_CONFIRMATION_SAMPLES="));
  Serial.println(FLAME_CONFIRMATION_SAMPLES);
  Serial.print(F("[CFG] AUTO_CALIBRATE_FLAME_SAFE_LEVEL="));
  Serial.println(AUTO_CALIBRATE_FLAME_SAFE_LEVEL ? F("true") : F("false"));
  Serial.print(F("[CFG] FLAME_CALIBRATION_MS="));
  Serial.println(FLAME_CALIBRATION_MS);
  Serial.print(F("[CFG] FLAME_SENSOR_ENABLED=["));
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i) Serial.print(',');
    Serial.print(FLAME_SENSOR_ENABLED[i] ? '1' : '0');
  }
  Serial.println(']');
  Serial.print(F("[CFG] MANUAL_FLAME_DETECTED_LEVELS=["));
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i) Serial.print(',');
    Serial.print(MANUAL_FLAME_DETECTED_LEVELS[i] == HIGH ? 'H' : 'L');
  }
  Serial.println(']');
  Serial.print(F("[CFG] SMOKE_SENSOR_ENABLED=["));
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i) Serial.print(',');
    Serial.print(SMOKE_SENSOR_ENABLED[i] ? '1' : '0');
  }
  Serial.println(']');
  Serial.print(F("[CFG] SMOKE_THRESHOLDS=["));
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i) Serial.print(',');
    Serial.print(SMOKE_THRESHOLDS[i]);
  }
  Serial.println(']');
  Serial.print(F("[CFG] USE_SMOKE_BASELINE_THRESHOLD="));
  Serial.print(USE_SMOKE_BASELINE_THRESHOLD ? F("YES delta=") : F("no (absolute thresholds) delta="));
  Serial.println(SMOKE_DELTA_THRESHOLD);
  Serial.print(F("[CFG] SMOKE_STARTUP_IGNORE_MS="));
  Serial.println(SMOKE_STARTUP_IGNORE_MS);
  Serial.println(F("[CFG] MQ-2 smoke alerts are SUPPRESSED during the warm-up window above."));
  if (COMMISSIONING_MODE) {
    Serial.println(F("[CFG] *** REMEMBER: set COMMISSIONING_MODE=false for production ***"));
  } else {
    Serial.println(F("[CFG] If buzzer is on at boot, read the [TELEM] lines and flip the wrong constant."));
  }

  // Pin modes
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    pinMode(FLAME_PINS[i], INPUT);

    // Seed the flame interpretation tables. Three cases:
    //   1. USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING=true OR
    //      AUTO_CALIBRATE_FLAME_SAFE_LEVEL=false
    //        -> manual levels are authoritative immediately. No tally window.
    //   2. Auto-cal enabled (default safety mode)
    //        -> seed provisionally from the first read; readSensors()
    //           finalizes after FLAME_CALIBRATION_MS using majority vote.
    int seedRaw = digitalRead(FLAME_PINS[i]);
    bool manualLevelsAuthoritative =
        !AUTO_CALIBRATE_FLAME_SAFE_LEVEL ||
        USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING;
    if (manualLevelsAuthoritative) {
      flameDetectedLevels[i] = MANUAL_FLAME_DETECTED_LEVELS[i];
      flameSafeLevels[i]     = (MANUAL_FLAME_DETECTED_LEVELS[i] == HIGH) ? LOW : HIGH;
    } else {
      flameSafeLevels[i]     = seedRaw;
      flameDetectedLevels[i] = (seedRaw == HIGH) ? LOW : HIGH;
    }

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

  // Skip the calibration window if manual levels are authoritative — either
  // because AUTO_CALIBRATE_FLAME_SAFE_LEVEL is false, or because the
  // commissioning override is on.
  bool skipFlameCalibration =
      !AUTO_CALIBRATE_FLAME_SAFE_LEVEL ||
      USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING;
  flameCalibrationComplete = skipFlameCalibration;
  // NOTE: flameCalibrationStartedAt is set at the END of setup() so the 5 s
  // window runs during loop() iterations (where samples are actually taken)
  // rather than expiring during a blocking WiFi.begin() in setup().
  if (!skipFlameCalibration) {
    Serial.print(F("[CAL] Auto-calibrating flame safe levels for the first "));
    Serial.print(FLAME_CALIBRATION_MS / 1000);
    Serial.println(F(" s of operation."));
    Serial.println(F("[CAL] Keep all flame sensors AWAY from flame during this window."));
  } else if (USE_MANUAL_FLAME_LEVELS_DURING_COMMISSIONING) {
    Serial.println(F("[CAL] Auto-cal bypassed: MANUAL_FLAME_DETECTED_LEVELS[] is authoritative."));
  }
  // Buzzer is already parked at BUZZER_OFF_LEVEL by the pre-Serial init at
  // the very top of setup(), so we don't re-touch it here.

  // Try WiFi with a bounded wait. loop() will keep retrying if this fails.
  if (strcmp(SECRET_SSID, "YOUR_WIFI_SSID") == 0 ||
      strcmp(SECRET_PASS, "YOUR_WIFI_PASSWORD") == 0) {
    Serial.println(F("[WARN] WiFi credentials are still placeholders — set"));
    Serial.println(F("[WARN] SECRET_SSID/SECRET_PASS in arduino.ino. Continuing"));
    Serial.println(F("[WARN] without network so the safety loop still runs."));
  }
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

  lastWifiAttempt           = millis();
  bootMs                    = millis();
  flameCalibrationStartedAt = millis();
}

void loop() {
  uint32_t now = millis();

  // BUZZER HARDWARE PROOF — runs before BUZZER_DIAGNOSTIC_MODE,
  // sensors, WiFi, HTTP, telemetry. Re-asserts D12 every 500 ms (or
  // prints a status heartbeat every 2 s for FLOAT). Returns early so
  // nothing else touches the pin or the rest of the rig.
  {
    const char *proofMode = nullptr;
    if (buzzerProofActive(proofMode)) {
      static uint32_t lastProofMs = 0;
      bool isFloat = (strcmp(proofMode, "FLOAT") == 0);
      uint32_t interval = isFloat ? 2000 : 500;
      if ((now - lastProofMs) >= interval) {
        lastProofMs = now;
        applyBuzzerProof(proofMode, /*firstCall=*/false);
        if (isFloat) {
          Serial.println(F("[BUZZER_PROOF] mode=FLOAT pinMode=INPUT (D12 high-Z)"));
        } else {
          Serial.print(F("[BUZZER_PROOF] mode="));
          Serial.print(proofMode);
          Serial.print(F(" pinLevel="));
          Serial.println(lastBuzzerPinLevel == HIGH ? F("HIGH") : F("LOW"));
        }
      }
      return;
    }
  }

  // Hardware-isolation mode: bypass every sensor / alert / WiFi / HTTP
  // path and just blink D12 in a known pattern. If the physical buzzer
  // does not follow this pattern, the buzzer is wired wrong (different
  // pin, no signal connection, or directly across 5V/GND). See the
  // [CFG] BUZZER_DIAGNOSTIC_MODE notes at the top of the file.
  if (BUZZER_DIAGNOSTIC_MODE) {
    static uint32_t diagPhaseStart = 0;
    static bool     diagOn         = false;
    if (diagPhaseStart == 0) {
      diagPhaseStart = now;
      digitalWrite(BUZZER_PIN, BUZZER_INACTIVE_LEVEL);
      lastBuzzerPinLevel = BUZZER_INACTIVE_LEVEL;
      Serial.println(F("[BUZZER_TEST] D12=INACTIVE"));
    }
    unsigned long phaseMs = diagOn ? BUZZER_DIAGNOSTIC_ON_MS : BUZZER_DIAGNOSTIC_OFF_MS;
    if ((now - diagPhaseStart) >= phaseMs) {
      diagOn = !diagOn;
      diagPhaseStart = now;
      int level = diagOn ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL;
      digitalWrite(BUZZER_PIN, level);
      lastBuzzerPinLevel = level;
      Serial.println(diagOn ? F("[BUZZER_TEST] D12=ACTIVE")
                            : F("[BUZZER_TEST] D12=INACTIVE"));
    }
    return; // skip WiFi maintenance, sensors, alerts, HTTP, telemetry
  }

  maintainWifi(now);
  readSensors(now);
  updateAlerts(now);
  handleHttpClient();
  logTelemetry(now);
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
// Three-branch flame read per room:
//   (1) disabled  -> force flameRaw false, keep smoke sampling
//   (2) calibrating (auto-cal only) -> tally HIGH/LOW counts, do NOT detect
//   (3) normal -> flameRaw = (digitalRead == flameDetectedLevels[i])
// After every room is sampled, finalize the auto-cal window if it has just
// elapsed. The existing debounce / lockout / alert machinery in updateAlerts()
// runs unchanged downstream — it sees flameRaw=false during cal so no buzzer
// can fire until a real, post-calibration flame event survives debounce.
void readSensors(uint32_t now) {
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    // Flame (digital, polarity-normalized). YL-38/KY-026 modules drive D0 with
    // an LM393 push-pull comparator, so plain INPUT is correct — INPUT_PULLUP
    // would fight the comparator's HIGH side.
    int raw = digitalRead(FLAME_PINS[i]);
    flameRawLevel[i] = raw; // cached for logTelemetry() + sendJsonStatus()

    if (!FLAME_SENSOR_ENABLED[i]) {
      // Disabled sensor: never trigger flame for this room. Smoke still works.
      if (rooms[i].flameRaw) {
        rooms[i].flameRaw          = false;
        rooms[i].flameLastChangeMs = now;
      }
    } else if (AUTO_CALIBRATE_FLAME_SAFE_LEVEL && !flameCalibrationComplete) {
      // Calibration window: tally samples per pin and hold flameRaw=false so
      // debounce cannot promote any reading to flameDetected.
      if (raw == HIGH) flameHighCount[i]++;
      else             flameLowCount[i]++;
      if (rooms[i].flameRaw) {
        rooms[i].flameRaw          = false;
        rooms[i].flameLastChangeMs = now;
      }
    } else {
      bool active = (raw == flameDetectedLevels[i]);
      if (active != rooms[i].flameRaw) {
        rooms[i].flameRaw          = active;
        rooms[i].flameLastChangeMs = now;
      }
    }

    // Smoke (analog, smoothed) — unaffected by flame branch. We always sample
    // (so disabled rooms still show a useful raw value in telemetry) but the
    // alert decision in updateAlerts() gates on SMOKE_SENSOR_ENABLED[i].
    uint16_t s = analogRead(SMOKE_PINS[i]);
    rooms[i].smokeBuf[rooms[i].smokeIdx] = s;
    rooms[i].smokeIdx = (rooms[i].smokeIdx + 1) % SMOKE_SAMPLES;
    uint32_t sum = 0;
    for (uint8_t k = 0; k < SMOKE_SAMPLES; k++) sum += rooms[i].smokeBuf[k];
    rooms[i].smokeAvg = sum / SMOKE_SAMPLES;

    // Smoke baseline capture: while the warm-up window is still open and the
    // sensor is enabled, accumulate samples so updateAlerts() can finalize a
    // per-room baseline at the warm-up edge. We use the smoothed smokeAvg
    // rather than the raw read so the baseline already includes the rolling
    // average's smoothing.
    if (SMOKE_SENSOR_ENABLED[i] && !smokeBaselineReady) {
      smokeBaselineSum[i]     += rooms[i].smokeAvg;
      smokeBaselineSamples[i] += 1;
    }

    // Edge-rate + stuck-pin diagnostics for the flame inputs (post-cal only).
    // A genuinely toggling input (potentiometer near threshold, floating wire,
    // bad ground) shows up as many transitions per 5-second window even when
    // there is no fire. A genuinely stuck input sits at the detected level
    // without the alert ever confirming — that's a wiring/pot problem, not a
    // fire. Both warnings fire at most once per boot so the log stays clean.
    if (FLAME_SENSOR_ENABLED[i] && flameCalibrationComplete) {
      static int prevFlameSample[ROOM_COUNT] = {0, 0, 0, 0, 0};
      if (raw != prevFlameSample[i]) {
        flameEdgesInWindow[i]++;
        flameLastEdgeMs[i] = now;
        prevFlameSample[i] = raw;
      }
      if ((now - flameNoiseWindowStart[i]) >= 5000) {
        if (flameEdgesInWindow[i] > 20 && !flameNoisyWarned[i]) {
          Serial.print(F("[WARN] room="));
          Serial.print(i + 1);
          Serial.print(F(" flame input is noisy (edges="));
          Serial.print(flameEdgesInWindow[i]);
          Serial.println(F(" in 5s); check potentiometer / common ground / wiring"));
          flameNoisyWarned[i] = true;
        }
        flameNoiseWindowStart[i] = now;
        flameEdgesInWindow[i]    = 0;
      }
      if (raw == flameDetectedLevels[i]) {
        if (flameDetectedSinceMs[i] == 0) flameDetectedSinceMs[i] = now;
        if (!flameStuckPostCalWarned[i] && !rooms[i].flameDetected &&
            (now - flameDetectedSinceMs[i]) >= 10000) {
          Serial.print(F("[WARN] room="));
          Serial.print(i + 1);
          Serial.println(F(" flame input stuck at detected level (>10s) without confirming a fire"));
          flameStuckPostCalWarned[i] = true;
        }
      } else {
        flameDetectedSinceMs[i] = 0;
      }
    }
  }

  // Finalize the calibration window once it has just elapsed. Majority vote
  // picks the safe baseline; detected level is its opposite. After locking the
  // result, re-read each pin once and warn loudly for any sensor that is
  // already reading "detected" — that's the signature of a floating /
  // miswired / over-sensitive module that would otherwise latch the buzzer.
  if (AUTO_CALIBRATE_FLAME_SAFE_LEVEL && !flameCalibrationComplete &&
      (now - flameCalibrationStartedAt) >= FLAME_CALIBRATION_MS) {
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      if (!FLAME_SENSOR_ENABLED[i]) {
        Serial.print(F("[CAL] room="));
        Serial.print(i + 1);
        Serial.println(F(" SKIPPED (FLAME_SENSOR_ENABLED=false)"));
        continue;
      }
      int safe = (flameHighCount[i] >= flameLowCount[i]) ? HIGH : LOW;
      flameSafeLevels[i]     = safe;
      flameDetectedLevels[i] = (safe == HIGH) ? LOW : HIGH;
      Serial.print(F("[CAL] room="));
      Serial.print(i + 1);
      Serial.print(F(" safe="));
      Serial.print(safe == HIGH ? F("H") : F("L"));
      Serial.print(F(" detected="));
      Serial.print(flameDetectedLevels[i] == HIGH ? F("H") : F("L"));
      Serial.print(F(" highCount="));
      Serial.print(flameHighCount[i]);
      Serial.print(F(" lowCount="));
      Serial.println(flameLowCount[i]);
    }
    flameCalibrationComplete = true;
    Serial.println(F("[CAL] flame baseline learned"));

    // Fresh debounce arming so the first post-cal read cannot satisfy a stale
    // 250 ms window. Without this, flameLastChangeMs was 0 (well past the
    // debounce threshold), and a single matching post-cal sample on a noisy
    // pin could promote straight to flameDetected and latch the buzzer.
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      if (!FLAME_SENSOR_ENABLED[i]) continue;
      rooms[i].flameRaw          = false;
      rooms[i].flameLastChangeMs = now;
      flameConfirmStreak[i]      = 0;
      flameNoiseWindowStart[i]   = now;
      flameEdgesInWindow[i]      = 0;
      flameDetectedSinceMs[i]    = 0;

      int postRaw = digitalRead(FLAME_PINS[i]);
      flameRawLevel[i] = postRaw;
      if (postRaw == flameDetectedLevels[i] && !flameStuckWarningShown[i]) {
        Serial.print(F("[WARN] room="));
        Serial.print(i + 1);
        Serial.println(F(" flame pin reads DETECTED right after calibration."));
        Serial.println(F("[WARN]  -> check wiring, potentiometer, or set"));
        Serial.println(F("[WARN]     FLAME_SENSOR_ENABLED[i]=false temporarily."));
        flameStuckWarningShown[i] = true;
      }
    }
  }
}

// ============================================================
// Alert state machine
// ============================================================
// Computes per-room flame/smoke detection, then branches on COMMISSIONING_MODE:
//   * Commissioning: report `wouldAlert` in telemetry, but do not latch fire
//     lockout, do not cut lights, do not energise the buzzer. Lets the user
//     iterate on sensor wiring/thresholds without ever firing the alarm.
//   * Safety mode (production): existing behavior — flame rising edge cuts
//     the light + latches `fireLockout`, smoke threshold cross fires a
//     one-shot notification, buzzer follows the combined alert. If a sensor
//     clears but a lockout is still held, buzzerReason becomes "latched".
void updateAlerts(uint32_t now) {
  bool warmedUp = (now - bootMs) >= SMOKE_STARTUP_IGNORE_MS;

  // Finalize smoke baselines at the warm-up edge — once per boot.
  if (warmedUp && !smokeBaselineReady) {
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      if (smokeBaselineSamples[i] > 0) {
        smokeBaseline[i] = (int)(smokeBaselineSum[i] / smokeBaselineSamples[i]);
      } else {
        smokeBaseline[i] = 0;
      }
    }
    smokeBaselineReady = true;
    Serial.print(F("[CAL] smoke baselines:"));
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      Serial.print(F(" room"));
      Serial.print(i + 1);
      Serial.print('=');
      if (SMOKE_SENSOR_ENABLED[i]) Serial.print(smokeBaseline[i]);
      else                         Serial.print(F("(disabled)"));
    }
    Serial.println();
  }

  bool wouldHaveFlameAlert = false;
  bool wouldHaveSmokeAlert = false;

  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    Room &r = rooms[i];

    // Safety: a disabled flame sensor must never hold a lockout or pending
    // notification. readSensors() already zeroes flameRaw; we clear the
    // downstream state here so re-enabling at runtime is clean.
    if (!FLAME_SENSOR_ENABLED[i]) {
      r.fireLockout      = false;
      r.notifiedFire     = false;
      r.fireAlertSinceMs = 0;
    }

    // --- Flame: debounce + N-of-N confirmation.
    // Stage 1: the raw line must hold the new state for FLAME_DEBOUNCE_MS.
    // Stage 2: after the time threshold is satisfied, additionally require
    // FLAME_CONFIRMATION_SAMPLES consecutive loop iterations that still agree
    // before promoting to flameDetected. The promotion runs in both modes so
    // commissioning telemetry shows what the safety mode WOULD see; the
    // side-effects (lockout, light cut, notification) are guarded below.
    bool prev = r.flameDetected;
    if (r.flameRaw != r.flameDetected) {
      if ((now - r.flameLastChangeMs) >= FLAME_DEBOUNCE_MS) {
        flameConfirmStreak[i]++;
        if (flameConfirmStreak[i] >= FLAME_CONFIRMATION_SAMPLES) {
          r.flameDetected = r.flameRaw;
          flameConfirmStreak[i] = 0;
        }
      }
    } else {
      flameConfirmStreak[i] = 0;
    }

    if (!COMMISSIONING_MODE) {
      // Rising edge: fire just confirmed -> system-wide cut-off + lockout.
      // Any room's fire kills every light and latches every room's lockout
      // so the visual "all dark" alarm covers the whole building until a
      // human acknowledges via /reset-alert.
      if (r.flameDetected && !prev) {
        Serial.print(F("[FIRE] room="));
        Serial.println(i + 1);
        tripAllLightsForHazard();
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
    }

    // --- Smoke: per-room threshold or baseline+delta. Disabled rooms are
    // still sampled (for telemetry) but never produce a smoke alert.
    bool smokeOver = false;
    if (SMOKE_SENSOR_ENABLED[i] && warmedUp) {
      if (USE_SMOKE_BASELINE_THRESHOLD && smokeBaselineReady) {
        smokeOver = ((int)r.smokeAvg) > (smokeBaseline[i] + SMOKE_DELTA_THRESHOLD);
      } else {
        smokeOver = ((int)r.smokeAvg) > SMOKE_THRESHOLDS[i];
      }
    }

    if (!SMOKE_SENSOR_ENABLED[i]) {
      // Disabled — guarantee no smoke side effects.
      if (r.smokeAlert) {
        r.smokeAlert    = false;
        r.notifiedSmoke = false;
      }
    } else if (smokeOver && !r.smokeAlert) {
      r.smokeAlert        = true;
      r.smokeAlertSinceMs = now;
      r.notifiedSmoke     = false;
      if (!COMMISSIONING_MODE) lastAlertRoom = i;
      Serial.print(F("[SMOKE] room="));
      Serial.print(i + 1);
      Serial.print(F(" raw="));
      Serial.print(r.smokeAvg);
      Serial.print(F(" threshold="));
      Serial.print(SMOKE_THRESHOLDS[i]);
      if (USE_SMOKE_BASELINE_THRESHOLD && smokeBaselineReady) {
        Serial.print(F(" baseline="));
        Serial.print(smokeBaseline[i]);
        Serial.print(F(" delta="));
        Serial.print((int)r.smokeAvg - smokeBaseline[i]);
      }
      Serial.print(F(" warmup="));
      Serial.print(warmedUp ? F("no") : F("yes"));
      Serial.println(F(" alert=YES"));
      // Smoke now triggers the same building-wide hazard trip as fire:
      // every light is cut and every room's fireLockout is latched so the
      // visual "all dark" alarm holds until /reset-alert clears it.
      if (!COMMISSIONING_MODE) {
        Serial.print(F("[SMOKE_TRIP] room="));
        Serial.println(i + 1);
        tripAllLightsForHazard();
      }
    } else if (!smokeOver && r.smokeAlert) {
      r.smokeAlert    = false;
      r.notifiedSmoke = false; // allow re-notify on next crossing
      Serial.print(F("[SMOKE] room="));
      Serial.print(i + 1);
      Serial.print(F(" raw="));
      Serial.print(r.smokeAvg);
      Serial.print(F(" threshold="));
      Serial.print(SMOKE_THRESHOLDS[i]);
      if (USE_SMOKE_BASELINE_THRESHOLD && smokeBaselineReady) {
        Serial.print(F(" baseline="));
        Serial.print(smokeBaseline[i]);
      }
      Serial.print(F(" warmup="));
      Serial.print(warmedUp ? F("no") : F("yes"));
      Serial.println(F(" alert=no"));
    }
    if (!COMMISSIONING_MODE && r.smokeAlert && !r.notifiedSmoke &&
        (now - r.smokeAlertSinceMs) >= NOTIFICATION_DELAY_MS) {
      sendAlertNotification(i, "smoke");
      r.notifiedSmoke = true;
    }

    if (FLAME_SENSOR_ENABLED[i] && r.flameDetected) wouldHaveFlameAlert = true;
    if (SMOKE_SENSOR_ENABLED[i] && r.smokeAlert)    wouldHaveSmokeAlert = true;
  }

  bool wouldHaveSensorAlert = wouldHaveFlameAlert || wouldHaveSmokeAlert;
  wouldAlert = wouldHaveSensorAlert;

  const char *sensorReason = "off";
  if      (wouldHaveFlameAlert && wouldHaveSmokeAlert) sensorReason = "flame+smoke";
  else if (wouldHaveFlameAlert)                        sensorReason = "flame";
  else if (wouldHaveSmokeAlert)                        sensorReason = "smoke";

  if (COMMISSIONING_MODE) {
    // Clear any latches that survived a previous safety-mode boot. This means
    // toggling COMMISSIONING_MODE=true is a soft "reset everything" — useful
    // when you flash a new build to silence a stuck alarm.
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      if (rooms[i].fireLockout) rooms[i].fireLockout = false;
      rooms[i].notifiedFire    = false;
      rooms[i].notifiedSmoke   = false;
    }
    globalAlert      = false;
    alarmStartedAt   = 0;
    buzzerTimedOut   = false;
    prevHazardActive = false;
    buzzerOnUntilMs  = 0;
    setBuzzer(false, wouldHaveSensorAlert ? "commissioning_suppressed" : "off");
  } else {
    // Safety mode (new auto-silence model):
    //   * currentHazard tracks CURRENT sensor readings, not the latched
    //     fireLockout. The buzzer follows currentHazard, so a stuck latch
    //     can no longer hold the buzzer on forever.
    //   * On the rising edge of currentHazard we record alarmStartedAt and
    //     clear buzzerTimedOut. The buzzer runs for up to BUZZER_ALARM_MS
    //     and then auto-silences (buzzerTimedOut=true). Lights stay OFF
    //     because fireLockout is still latched.
    //   * On the falling edge (sensors clear) we re-arm by zeroing the
    //     timeout state so the NEXT hazard can sound again.
    //   * globalAlert (used by /status and the app's red banner) still
    //     reflects "system is alarming" — it includes anyLockout — so the
    //     UI shows an active alarm even when the buzzer has timed out.
    bool currentHazard = wouldHaveSensorAlert;

    if (currentHazard && !prevHazardActive) {
      alarmStartedAt = now;
      buzzerTimedOut = false;
      Serial.print(F("[ALARM] start reason="));
      Serial.println(sensorReason);
    }
    if (!currentHazard && prevHazardActive) {
      buzzerTimedOut  = false;
      alarmStartedAt  = 0;
      buzzerOnUntilMs = 0;
      Serial.println(F("[ALARM] sensor cleared (lockout still latched if any)"));
    }
    if (currentHazard && !buzzerTimedOut && BUZZER_ALARM_MS > 0 &&
        (now - alarmStartedAt) >= BUZZER_ALARM_MS) {
      buzzerTimedOut = true;
      Serial.print(F("[ALARM] buzzer auto-silenced after "));
      Serial.print(BUZZER_ALARM_MS / 1000);
      Serial.println(F(" s"));
    }
    prevHazardActive = currentHazard;

    bool anyLockout = false;
    for (uint8_t i = 0; i < ROOM_COUNT; i++) {
      if (rooms[i].fireLockout) { anyLockout = true; break; }
    }
    bool actualAlert      = currentHazard || anyLockout;
    bool buzzerShouldBeOn = currentHazard && !buzzerTimedOut;
    globalAlert     = actualAlert;
    buzzerOnUntilMs = buzzerShouldBeOn ? (alarmStartedAt + BUZZER_ALARM_MS) : 0;

    const char *reason;
    if      (buzzerShouldBeOn)                 reason = sensorReason;   // flame / smoke / flame+smoke
    else if (currentHazard && buzzerTimedOut)  reason = "timed_out";
    else if (anyLockout)                       reason = "latched";      // lights still off, buzzer silent
    else                                       reason = "off";
    setBuzzer(buzzerShouldBeOn, reason);
  }
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

// Returns true if any BUZZER_*_TEST is enabled and assigns selectedTest
// to one of "LOW" / "HIGH" / "FLOAT". If more than one is enabled,
// prints an error and defaults to "LOW" so the safe inactive level is
// asserted while the user fixes the source.
bool buzzerProofActive(const char *&selectedTest) {
  uint8_t enabled = (BUZZER_FORCE_PIN_LOW_TEST  ? 1 : 0)
                  + (BUZZER_FORCE_PIN_HIGH_TEST ? 1 : 0)
                  + (BUZZER_FLOAT_PIN_TEST      ? 1 : 0);
  if (enabled == 0) { selectedTest = nullptr; return false; }
  if (enabled > 1) {
    Serial.println(F("[BUZZER_PROOF][ERR] more than one proof mode is true;"));
    Serial.println(F("[BUZZER_PROOF][ERR] defaulting to FORCE_PIN_LOW."));
    selectedTest = "LOW";
    return true;
  }
  if (BUZZER_FORCE_PIN_LOW_TEST)  { selectedTest = "LOW";   return true; }
  if (BUZZER_FORCE_PIN_HIGH_TEST) { selectedTest = "HIGH";  return true; }
  selectedTest = "FLOAT";
  return true;
}

// Latches D12 according to the active proof mode. firstCall=true prints
// the one-time banner; subsequent calls re-assert silently. The pin is
// re-driven every 500 ms from loop() so any spurious external pull is
// visible against the known commanded state.
void applyBuzzerProof(const char *selectedTest, bool firstCall) {
  if (strcmp(selectedTest, "FLOAT") == 0) {
    pinMode(BUZZER_PIN, INPUT);
    if (firstCall) {
      Serial.println(F("[BUZZER_PROOF] D12 set to INPUT/floating."));
      Serial.println(F("[BUZZER_PROOF] If buzzer still beeps, wiring/module is not controlled correctly by D12."));
    }
  } else if (strcmp(selectedTest, "HIGH") == 0) {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, HIGH);
    lastBuzzerPinLevel = HIGH;
    if (firstCall) {
      Serial.println(F("[BUZZER_PROOF] D12 forced HIGH forever."));
      Serial.println(F("[BUZZER_PROOF] If buzzer stops here but beeps when LOW, buzzer is active-low."));
    }
  } else {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    lastBuzzerPinLevel = LOW;
    if (firstCall) {
      Serial.println(F("[BUZZER_PROOF] D12 forced LOW forever."));
      Serial.println(F("[BUZZER_PROOF] If buzzer beeps, it is active-low OR wired incorrectly."));
    }
  }
}

// True if any enabled flame sensor has confirmed flame (post-debounce).
bool anyFireDetected() {
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (FLAME_SENSOR_ENABLED[i] && rooms[i].flameDetected) return true;
  }
  return false;
}

// True if any enabled smoke sensor is currently above its threshold/delta.
bool anySmokeDetected() {
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (SMOKE_SENSOR_ENABLED[i] && rooms[i].smokeAlert) return true;
  }
  return false;
}

// Building-wide hazard trip: cut every light, latch lockout on every room.
// Idempotent — safe to call repeatedly. Used by BOTH flame and smoke rising
// edges so smoke produces identical observable safety behavior to fire
// (lights off + manual /light POST blocked + /reset-alert required).
void tripAllLightsForHazard() {
  for (uint8_t j = 0; j < ROOM_COUNT; j++) {
    setLight(j, false);
    rooms[j].fireLockout = true;
  }
}

// Single source of truth for "is the buzzer on right now and why". The
// `reason` argument is recorded into the global buzzerReason so /status and
// telemetry never disagree with the actual pin state. Diagnostic mode owns
// the pin in loop() — we leave the GPIO alone here so the test pattern is
// not fought by alert decisions.
void setBuzzer(bool on, const char *reason) {
  if (BUZZER_DIAGNOSTIC_MODE) {
    // Don't touch the pin; loop() is driving the blink pattern.
    buzzerReason = "diagnostic";
    return;
  }

  // BUZZER_FORCE_DISABLED short-circuits every active write while still
  // logging once on the falling edge from a previously-active state, so the
  // Serial Monitor shows that the alert path TRIED to fire and was
  // suppressed. The pin is held at INACTIVE.
  if (on && BUZZER_FORCE_DISABLED) {
    if (buzzerOn) {
      Serial.println(F("[BUZZER] force-disabled is ON; alert path suppressed"));
    }
    buzzerOn = false;
    buzzerReason = "force_disabled";
    if (lastBuzzerPinLevel != BUZZER_INACTIVE_LEVEL) {
      digitalWrite(BUZZER_PIN, BUZZER_INACTIVE_LEVEL);
      lastBuzzerPinLevel = BUZZER_INACTIVE_LEVEL;
    }
    return;
  }

  buzzerReason = reason;
  if (on == buzzerOn) return; // idempotent — avoid extra writes
  buzzerOn = on;
  int level = on ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL;
  digitalWrite(BUZZER_PIN, level);
  lastBuzzerPinLevel = level;
}

// ============================================================
// Telemetry — periodic Serial dump for diagnosing the buzzer
// ============================================================
// Throttled to once every ~2 s. Prints one global summary line followed by one
// line per room so the user can grep/scan and immediately see which room and
// which sensor is driving the alert. This is the primary diagnostic tool for
// the continuous-buzzer playbook documented at the top of the file.
void logTelemetry(uint32_t now) {
  static uint32_t lastLogMs = 0;
  if ((now - lastLogMs) < 2000) return;
  lastLogMs = now;

  bool warmedUp = (now - bootMs) >= SMOKE_STARTUP_IGNORE_MS;
  Serial.print(F("[TELEM] up="));
  Serial.print((now - bootMs) / 1000);
  Serial.print(F("s warm="));
  Serial.print(warmedUp ? F("yes") : F("no"));
  Serial.print(F(" calib="));
  Serial.print(flameCalibrationComplete ? F("done") : F("learning"));
  Serial.print(F(" smokeBaseline="));
  Serial.print(smokeBaselineReady ? F("ready") : F("N/A"));
  Serial.print(F(" wifi="));
  Serial.print(WiFi.status() == WL_CONNECTED ? F("up") : F("down"));
  Serial.print(F(" commissioning="));
  Serial.print(COMMISSIONING_MODE ? F("YES") : F("no"));
  bool fireNow  = anyFireDetected();
  bool smokeNow = anySmokeDetected();
  Serial.print(F(" wouldAlert="));
  Serial.print(wouldAlert ? F("YES") : F("no"));
  Serial.print(F(" actualAlert="));
  Serial.print(globalAlert ? F("YES") : F("no"));
  Serial.print(F(" alarmActive="));
  Serial.print((fireNow || smokeNow) ? F("YES") : F("no"));
  Serial.print(F(" fireDetected="));
  Serial.print(fireNow  ? F("YES") : F("no"));
  Serial.print(F(" smokeDetected="));
  Serial.print(smokeNow ? F("YES") : F("no"));
  Serial.print(F(" buzzerTimedOut="));
  Serial.print(buzzerTimedOut ? F("YES") : F("no"));
  Serial.print(F(" buzzerOnUntilMs="));
  Serial.print(buzzerOnUntilMs);
  Serial.print(F(" buzzerCmd="));
  Serial.print(buzzerOn ? F("ON") : F("OFF"));
  Serial.print(F(" buzzerPinWrite="));
  Serial.print(lastBuzzerPinLevel == HIGH ? F("HIGH") : F("LOW"));
  Serial.print(F(" buzzerForceDisabled="));
  Serial.print(BUZZER_FORCE_DISABLED ? F("Y") : F("N"));
  Serial.print(F(" buzzer="));
  Serial.print(buzzerOn ? F("ON") : F("off"));
  Serial.print(F(" reason="));
  Serial.println(buzzerReason);

  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    // Use the cached read from readSensors() so this telemetry line cannot
    // disagree with the flameRaw= / flame= fields below: all three reporters
    // see the same physical sample within one loop iteration.
    int raw = flameRawLevel[i];
    Serial.print(F("[TELEM] room="));
    Serial.print(i + 1);
    Serial.print(F(" raw="));
    Serial.print(raw == HIGH ? F("H") : F("L"));
    Serial.print(F(" safe="));
    Serial.print(flameSafeLevels[i] == HIGH ? F("H") : F("L"));
    Serial.print(F(" det="));
    Serial.print(flameDetectedLevels[i] == HIGH ? F("H") : F("L"));
    Serial.print(F(" flameEn="));
    Serial.print(FLAME_SENSOR_ENABLED[i] ? F("y") : F("n"));
    Serial.print(F(" flameRaw="));
    Serial.print(rooms[i].flameRaw ? F("YES") : F("no"));
    Serial.print(F(" flame="));
    Serial.print(rooms[i].flameDetected ? F("YES") : F("no"));
    Serial.print(F(" mq2="));
    Serial.print(rooms[i].smokeAvg);
    Serial.print(F(" threshold="));
    Serial.print(SMOKE_THRESHOLDS[i]);
    Serial.print(F(" baseline="));
    if (smokeBaselineReady && SMOKE_SENSOR_ENABLED[i]) {
      Serial.print(smokeBaseline[i]);
    } else {
      Serial.print(F("N/A"));
    }
    Serial.print(F(" delta="));
    if (smokeBaselineReady && SMOKE_SENSOR_ENABLED[i]) {
      Serial.print((int)rooms[i].smokeAvg - smokeBaseline[i]);
    } else {
      Serial.print(F("N/A"));
    }
    Serial.print(F(" smokeEn="));
    Serial.print(SMOKE_SENSOR_ENABLED[i] ? F("y") : F("n"));
    Serial.print(F(" smoke="));
    Serial.print(rooms[i].smokeAlert ? F("YES") : F("no"));
    Serial.print(F(" light="));
    Serial.print(rooms[i].lightOn ? F("on") : F("off"));
    Serial.print(F(" lockout="));
    Serial.println(rooms[i].fireLockout ? F("YES") : F("no"));

    if (FLAME_SENSOR_ENABLED[i] && rooms[i].flameRaw && raw == flameDetectedLevels[i]) {
      Serial.print(F("[CAUSE] room="));
      Serial.print(i + 1);
      Serial.print(F(" flame would alert because raw="));
      Serial.print(raw == HIGH ? F("H") : F("L"));
      Serial.print(F(" equals detected="));
      Serial.print(flameDetectedLevels[i] == HIGH ? F("H") : F("L"));
      Serial.println(F(" (flip MANUAL_FLAME_DETECTED_LEVELS or disable this room to silence)"));
    }
  }
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
  globalAlert      = false;
  lastAlertRoom    = -1;
  alarmStartedAt   = 0;
  buzzerTimedOut   = false;
  prevHazardActive = false;
  buzzerOnUntilMs  = 0;
  setBuzzer(false, "off");
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
      if (COMMISSIONING_MODE) {
        // Commissioning soft-reset: clear everything regardless of current
        // sensor state. Safe because no actuators were latched anyway, and
        // it gives the user a one-call way to silence anything that was
        // already noisy when they entered commissioning mode.
        for (uint8_t i = 0; i < ROOM_COUNT; i++) {
          rooms[i].fireLockout   = false;
          rooms[i].notifiedFire  = false;
          rooms[i].smokeAlert    = false;
          rooms[i].notifiedSmoke = false;
        }
        globalAlert      = false;
        lastAlertRoom    = -1;
        alarmStartedAt   = 0;
        buzzerTimedOut   = false;
        prevHazardActive = false;
        buzzerOnUntilMs  = 0;
        setBuzzer(false, "off");
        Serial.println(F("[RESET] alerts cleared (commissioning mode)"));
        sendResponse(httpClient, 200, "application/json",
                     "{\"ok\":true,\"message\":\"Commissioning mode: alerts cleared/suppressed\"}");
      } else if (resetAlertsIfSafe()) {
        sendResponse(httpClient, 200, "application/json", "{\"ok\":true,\"reset\":true}");
      } else {
        // Reset denied — surface exactly which rooms are still in danger so the
        // mobile app can show the user where to look instead of a generic error.
        // Disabled flame sensors cannot block reset (readSensors() forces their
        // flameRaw/flameDetected to false anyway — this is belt-and-braces).
        String body = "{\"ok\":false,\"message\":\"Cannot reset while danger is active\",\"blockingRooms\":[";
        bool first = true;
        for (uint8_t i = 0; i < ROOM_COUNT; i++) {
          if (FLAME_SENSOR_ENABLED[i] &&
              (rooms[i].flameDetected || rooms[i].flameRaw)) {
            if (!first) body += ",";
            body += "{\"room\":";
            body += (i + 1);
            body += ",\"reason\":\"flame\"}";
            first = false;
          }
          if (rooms[i].smokeAlert) {
            if (!first) body += ",";
            body += "{\"room\":";
            body += (i + 1);
            body += ",\"reason\":\"smoke\"}";
            first = false;
          }
        }
        body += "]}";
        sendResponse(httpClient, 409, "application/json", body);
      }
    } else if (method == "GET" && path == "/debug/buzzer") {
      // Benchtop diagnostic: directly drive D12 via setBuzzer() without going
      // through the alert state machine. Lets the user test from a phone or
      // browser whether the firmware can actually silence or sound the
      // buzzer. Response surfaces the actual GPIO level written so a polarity
      // mismatch (buzzer module is active-LOW but BUZZER_ACTIVE_LEVEL=HIGH)
      // is visible without a logic probe. GET is intentionally not token-
      // gated, matching /health and /status. LAN-only prototype.
      String want = queryParam(query, "state");
      bool requestOn = (want == "on" || want == "1" || want == "true");
      setBuzzer(requestOn, requestOn ? "diagnostic" : "off");
      String body;
      body.reserve(192);
      body += "{\"ok\":true,\"requested\":\"";
      body += (requestOn ? "on" : "off");
      body += "\",\"pin\":";
      body += BUZZER_PIN;
      body += ",\"writtenLevel\":\"";
      body += (lastBuzzerPinLevel == HIGH ? "HIGH" : "LOW");
      body += "\",\"buzzerCmd\":";
      body += (buzzerOn ? "true" : "false");
      body += ",\"forceDisabled\":";
      body += (BUZZER_FORCE_DISABLED ? "true" : "false");
      body += ",\"activeLevel\":\"";
      body += (BUZZER_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
      body += "\"}";
      sendResponse(httpClient, 200, "application/json", body);
    } else {
      sendResponse(httpClient, 404, "application/json", "{\"error\":\"not found\"}");
    }
  }

  httpClient.stop();
  httpClientActive = false;
}

void sendJsonStatus(WiFiClient &client) {
  String body;
  body.reserve(1280);
  body += "{\"wifi\":{\"connected\":";
  body += (WiFi.status() == WL_CONNECTED ? "true" : "false");
  body += ",\"ip\":\"";
  IPAddress ip = WiFi.localIP();
  body += ip[0]; body += "."; body += ip[1]; body += "."; body += ip[2]; body += "."; body += ip[3];
  body += "\",\"rssi\":";
  body += (int)WiFi.RSSI();
  body += "},\"uptimeMs\":";
  body += (uint32_t)(millis() - bootMs);
  body += ",\"commissioningMode\":";
  body += (COMMISSIONING_MODE ? "true" : "false");
  body += ",\"wouldAlert\":";
  body += (wouldAlert ? "true" : "false");
  body += ",\"actualAlert\":";
  body += (globalAlert ? "true" : "false");
  body += ",\"buzzer\":";
  body += (buzzerOn ? "true" : "false");
  body += ",\"buzzerCommand\":";
  body += (buzzerOn ? "true" : "false");
  body += ",\"buzzerPinWrite\":\"";
  body += (lastBuzzerPinLevel == HIGH ? "HIGH" : "LOW");
  body += "\",\"buzzerReason\":\"";
  body += buzzerReason;
  body += "\",\"alertActive\":";
  body += (globalAlert ? "true" : "false");
  bool _fireNow  = anyFireDetected();
  bool _smokeNow = anySmokeDetected();
  body += ",\"alarmActive\":";
  body += ((_fireNow || _smokeNow) ? "true" : "false");
  body += ",\"fireDetected\":";
  body += (_fireNow ? "true" : "false");
  body += ",\"smokeDetected\":";
  body += (_smokeNow ? "true" : "false");
  body += ",\"buzzerTimedOut\":";
  body += (buzzerTimedOut ? "true" : "false");
  body += ",\"buzzerOnUntilMs\":";
  body += buzzerOnUntilMs;
  body += ",\"buzzerAlarmMs\":";
  body += (uint32_t)BUZZER_ALARM_MS;
  body += ",\"buzzerActiveHigh\":";
  body += (BUZZER_ACTIVE_HIGH ? "true" : "false");
  body += ",\"flameCalibrationComplete\":";
  body += (flameCalibrationComplete ? "true" : "false");
  body += ",\"smokeBaselineReady\":";
  body += (smokeBaselineReady ? "true" : "false");
  body += ",\"lastAlertRoom\":";
  // null when no alert has fired since boot/reset — UIs render this as "none"
  // instead of a misleading "Room 0".
  if (lastAlertRoom < 0) body += "null";
  else                   body += (lastAlertRoom + 1);
  body += ",\"rooms\":[";
  for (uint8_t i = 0; i < ROOM_COUNT; i++) {
    if (i > 0) body += ",";
    // Reuse the cached read from readSensors() so the HTTP /status response
    // matches the same physical sample the [TELEM] line is showing.
    int raw = flameRawLevel[i];
    body += "{\"room\":";
    body += (i + 1);
    body += ",\"lightOn\":";
    body += (rooms[i].lightOn ? "true" : "false");
    body += ",\"flameSensorEnabled\":";
    body += (FLAME_SENSOR_ENABLED[i] ? "true" : "false");
    body += ",\"smokeSensorEnabled\":";
    body += (SMOKE_SENSOR_ENABLED[i] ? "true" : "false");
    body += ",\"flameRaw\":\"";
    body += (raw == HIGH ? 'H' : 'L');
    body += "\",\"flameRawLevel\":\"";
    body += (raw == HIGH ? 'H' : 'L');
    body += "\",\"flameSafeLevel\":\"";
    body += (flameSafeLevels[i] == HIGH ? 'H' : 'L');
    body += "\",\"flameDetectedLevel\":\"";
    body += (flameDetectedLevels[i] == HIGH ? 'H' : 'L');
    body += "\",\"flameDetected\":";
    body += (rooms[i].flameDetected ? "true" : "false");
    body += ",\"fireLockout\":";
    body += (rooms[i].fireLockout ? "true" : "false");
    body += ",\"smokeValue\":";
    body += rooms[i].smokeAvg;
    body += ",\"smokeRaw\":";
    body += rooms[i].smokeAvg;
    body += ",\"smokeThreshold\":";
    body += SMOKE_THRESHOLDS[i];
    body += ",\"smokeBaseline\":";
    if (smokeBaselineReady && SMOKE_SENSOR_ENABLED[i]) body += smokeBaseline[i];
    else                                               body += "null";
    body += ",\"smokeDelta\":";
    if (smokeBaselineReady && SMOKE_SENSOR_ENABLED[i]) {
      body += ((int)rooms[i].smokeAvg - smokeBaseline[i]);
    } else {
      body += "null";
    }
    body += ",\"smokeDetected\":";
    body += (rooms[i].smokeAlert ? "true" : "false");
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
