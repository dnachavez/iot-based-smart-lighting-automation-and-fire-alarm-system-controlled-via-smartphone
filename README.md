# IoT Smart Lighting + Fire Alarm

Local-network fire alarm and lighting controller for an Arduino Uno R4 WiFi,
plus a React Native (Expo) phone app that polls the device, controls the
lights, and acts as the alarm.

The physical buzzer that lived on D12 in earlier revisions has been retired —
the phone is now the alarm. D12 is unused by firmware. The wiring diagram
still shows the buzzer footprint; leave it disconnected (or simply omit it
from new builds).

## Hardware

| Subsystem | Count | Wiring |
| --- | --- | --- |
| Flame sensors (digital DO) | 5 | D2..D6 |
| MQ-2 smoke sensors (analog) | 5 | A0..A4 |
| Relay-driven bulbs (220 V AC) | 5 | D7..D11 (active-LOW) |
| Buzzer (legacy, no longer driven) | — | D12 left unconnected |

The full schematic is in `.context/attachments/ogbcsK/ChatGPT Image May 25, 2026, 04_26_48 PM.png`.

> **Safety.** The relay module switches 220 V AC mains, which is lethal.
> Have a qualified electrician do the AC side. Flame and MQ-2 modules are
> hobby-grade; pair this device with a real UL/EN-listed smoke alarm. The
> phone app is not a certified alarm either — phones sleep, lose WiFi, and
> can be killed by the OS at any time.

## Firmware

The sketch is a single file: [`arduino.ino`](./arduino.ino). Target board:
**Arduino UNO R4 WiFi** (`arduino:renesas_uno:unor4wifi`).

### 1. Configure credentials

Edit the three macros near the top of `arduino.ino`:

```cpp
#define SECRET_SSID      "your-2.4ghz-network"
#define SECRET_PASS      "your-wifi-password"
#define SECRET_API_TOKEN "long-random-string"   // openssl rand -hex 32
```

The token must be at least 16 characters and not equal to the placeholder —
boot will warn loudly otherwise. The token gates every state-changing POST
(`/light`, `/all-lights`, `/reset-alert`).

### 2. Flash

Arduino IDE 2.x or `arduino-cli`:

```bash
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi .
arduino-cli upload  --fqbn arduino:renesas_uno:unor4wifi --port /dev/cu.usbmodem<your-port> .
```

### 3. Find the device IP

Open the Arduino IDE Serial Monitor at **115200 baud**. After boot you'll see:

```
[WIFI] connected ip=192.168.1.42 rssi=-55
[HTTP] listening port=80
```

That IP goes into the mobile app's Settings screen. (If the address is
inconvenient, reserve a static lease for the Arduino's MAC in your router.)

## HTTP API

All endpoints are HTTP/1.1, JSON, port 80, LAN-only, CORS-open. POSTs require
the shared token. The Arduino streams one request at a time and times out
silent clients after 2 s.

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| GET  | `/health` | — | Heartbeat. Returns `{"ok": true}`. |
| GET  | `/status` | — | Full state dump (schema below). |
| POST | `/light?room=1..5&state=on\|off&token=…` | token | Toggle one bulb. Returns 423 if room is in fire lockout. |
| POST | `/all-lights?state=off&token=…` | token | Force every bulb off. |
| POST | `/reset-alert?token=…` | token | Clear lockout if sensors read safe. Returns 409 with `blockingRooms` when a hazard is still active. |

### `/status` schema

```jsonc
{
  "wifi": { "connected": true, "ip": "192.168.1.42", "rssi": -55 },
  "uptimeMs": 12345,
  "commissioningMode": false,
  "wouldAlert": false,
  "actualAlert": false,
  "alertActive": false,
  "alarmActive": false,
  "episodeActive": false,
  "episodeTimedOut": false,
  "rearmed": true,
  "clearHoldRemainingMs": 0,
  "autoRestorePending": false,
  "restoreMode": "pre_trip_states",
  "alarmReason": "off",
  "alertEpisodeId": 0,
  "fireDetected": false,
  "smokeDetected": false,
  "rearmClearHoldMs": 2000,
  "autoRestoreOnSafe": true,
  "flameCalibrationComplete": true,
  "smokeBaselineReady": true,
  "lastAlertRoom": null,
  "rooms": [
    {
      "room": 1, "lightOn": false,
      "flameSensorEnabled": true, "smokeSensorEnabled": true,
      "flameRaw": "L", "flameRawLevel": "L",
      "flameSafeLevel": "H", "flameDetectedLevel": "L",
      "flameDetected": false, "fireLockout": false,
      "smokeValue": 178, "smokeRaw": 178,
      "smokeThreshold": 950, "smokeBaseline": 165, "smokeDelta": 13,
      "smokeDetected": false
    }
  ]
}
```

`alertEpisodeId` is the field the mobile app uses to dedupe its siren and
notifications. It increments on every rising edge into `episodeActive` and
never resets, even via `/reset-alert`.

`alarmReason` is one of: `off`, `commissioning_suppressed`, `flame`, `smoke`,
`flame+smoke`, `clear_hold`, `latched`.

## Mobile app

The Expo app lives under [`mobile/`](./mobile). It is a polling LAN client —
no cloud push, no broker.

### Run it

```bash
cd mobile
npm install        # only once
npx expo start
```

Scan the QR code with **Expo Go** on the phone (same WiFi as the Arduino), or
press `a` / `i` to launch an Android emulator / iOS simulator.

In the app: open **Settings**, enter the Arduino's IP + token, tap
**Test connection**, then **Save**.

See [`mobile/README.md`](./mobile/README.md) for the full app description and
verification checklist.

## Testing the alert flow

1. Flash the Arduino, wait for `[CAL] smoke baselines: …` in the Serial
   Monitor (~30 s after boot — the MQ-2 warm-up window).
2. Open the mobile app, confirm the dashboard shows `All clear` and live
   smoke values.
3. Wave a lighter near one of the flame sensors **briefly** (or briefly
   expose an MQ-2 to smoke).
4. Expected within ~1 s:
   - Serial Monitor logs `[ALARM] start id=1 reason=flame`.
   - All five lights cut to OFF (relays click).
   - Every room shows `fireLockout: true` in `/status`.
   - The mobile app's banner turns red, the siren loops, and the OS fires a
     local notification.
5. Remove the trigger. After two seconds of clean readings the firmware
   logs `[ALARM] clear-hold met; auto-restored pre-trip lights` and the
   app's banner returns to green.
6. Re-trigger to confirm a new `alertEpisodeId` (the app will fire a fresh
   notification — the previous one's mute does not carry over).

## Limitations

- **LAN-only**. The phone must be on the same WiFi as the Arduino. No relay
  service, no cloud push.
- **Polling latency**. The app reads `/status` every ~1.5 s, so the alarm
  has roughly that much delay even on a fast LAN.
- **Background**. iOS in particular aggressively pauses JavaScript timers
  when the app is backgrounded. The current MVP only polls in the
  foreground — backgrounded notifications are not reliable without an
  external push pipeline.
- **Single device**. Multiple phones can each poll, but they don't
  coordinate. The Arduino is the only authoritative episode state; muting
  the siren on one phone does not silence another.
- **Hobby-grade sensors**. KY-026 / YL-38 flame modules and MQ-2 smoke
  modules are not certified life-safety devices.

## Repo layout

```
.
├── arduino.ino                 # Firmware (single sketch)
├── mobile/                     # Expo (React Native) phone app
│   ├── App.tsx
│   ├── package.json
│   ├── app.json
│   ├── src/
│   │   ├── api/arduino.ts
│   │   ├── state/{Settings,Status}Context.tsx
│   │   ├── alert/{notifications,useAlarmSiren,useEpisodeNotifier,generateAlarmSound}.ts
│   │   ├── screens/{Dashboard,SettingsModal}.tsx
│   │   └── components/{Button,RoomCard,AlertBanner}.tsx
│   └── README.md
├── .context/attachments/…/wiring-diagram.png
└── README.md (this file)
```
