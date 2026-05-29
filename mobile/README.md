# Fire Alarm — Mobile app

Expo (React Native + TypeScript) phone app that polls the Arduino Uno R4
firmware over the local network, mirrors its `/status`, drives a phone-side
siren when a hazard episode begins, and posts a local OS notification.

## Quickstart

```bash
npm install
npx expo start
```

- Open in **Expo Go** on a phone connected to the same WiFi as the Arduino.
- In the app: tap **Settings**, enter the Arduino's IP and API token, tap
  **Test connection** (expects `Connected`), then **Save**.

## How it works

- `src/api/arduino.ts` — typed `fetch` client with `AbortController` timeouts.
  Tagged-union return type means callers can switch on `kind === 'timeout'`,
  `'network'`, `'http'`, or `'ok'` without `try/catch` sprawl.
- `src/state/StatusContext.tsx` — polls `GET /status` every 1.5 s while the
  app is foregrounded. Stops on background (`AppState.change`) and resumes on
  return to foreground so the OS doesn't kill the JS thread for being noisy.
- `src/state/SettingsContext.tsx` — persists IP + token to `AsyncStorage`.
  Mute is intentionally **not** persisted — every cold start begins unmuted.
- `src/alert/useEpisodeNotifier.ts` — single source of truth for "is this a
  new alert?". Uses the Arduino's monotonic `alertEpisodeId` as the dedup
  key. Each rising edge fires exactly one OS notification.
- `src/alert/useAlarmSiren.ts` — drives the looping in-app siren via
  `expo-av`. Mute silences only the current episode; a brand-new
  `alertEpisodeId` auto-unmutes so a fresh hazard always sounds.
- `src/alert/generateAlarmSound.ts` — synthesizes a 1.6 s alternating
  600/1100 Hz siren WAV at boot and base64-encodes it as a data URI. No
  binary audio asset is committed to the repo; the loop sounds on any
  device with audio output.

## Verification checklist

Run through this after `npm install` and a fresh `npx expo start`:

| # | Step | Expected |
| --- | --- | --- |
| 1 | Open Settings, enter IP/token, tap **Test connection** | "Connected" alert |
| 2 | Save settings | Dashboard polls and shows live smoke values |
| 3 | Tap a room's "Turn light on" | Relay clicks, status flips to `LIGHT ON` |
| 4 | Tap **All lights off** | All five room cards show `LIGHT OFF` |
| 5 | Trigger a flame sensor (lighter, brief) | Banner turns red, siren loops, OS notification fires |
| 6 | Tap **Mute alarm** | Siren stops; banner stays red |
| 7 | Move flame away; wait ~10 s | Banner returns to green; lights auto-restore |
| 8 | Re-trigger flame | New notification fires (new `alertEpisodeId`); siren plays even though we previously muted |
| 9 | Tap **Reset alert** while hazard still active | Alert "Cannot reset yet" with the blocking rooms |
| 10 | Background the app, foreground it after 30 s | Polling resumes; banner reflects current state |

If any step fails, check the Arduino Serial Monitor for `[FIRE]`, `[SMOKE]`,
`[ALARM]`, and `[TELEM]` lines — they show exactly which sensor is driving
the state machine.

## Known limitations

- **LAN-only.** No push backend, no MQTT broker. The phone must be on the
  same WiFi as the Arduino. The `app.json` sets iOS `NSAllowsLocalNetworking`
  and Android `usesCleartextTraffic` to permit plain `http://` to a LAN IP;
  a public cloud build would need TLS.
- **Polling has a floor of ~1.5 s**. That's the alarm latency on a healthy
  LAN. Lossy WiFi will be slower.
- **Background notifications are unreliable.** Both iOS and Android kill JS
  timers in the background; this MVP does not run a native background
  worker. For true 24/7 alerting you need a server-side push pipeline
  (FCM/APNS), which is out of scope.
- **Single-device.** No phone-to-phone coordination. The Arduino is the
  only authoritative state; muting on one phone doesn't silence another.
- **Sound asset is synthesized.** The siren is a two-tone WAV built in JS
  at startup. If you'd rather bundle a higher-fidelity tone, drop an mp3 in
  `assets/sounds/` and swap `ALARM_WAV_DATA_URI` for an asset `require(...)`
  in `useAlarmSiren.ts`.
- **Tested on Expo SDK 51 / React Native 0.74.** Newer SDKs may want
  `expo-audio` instead of `expo-av` — both expose the same play/loop/stop
  surface used here.

## Permissions

On first run the app requests **notification** permission. Without it,
hazards still appear in the dashboard and the in-app siren still plays —
you just won't get the OS notification banner.

The siren is configured with `playsInSilentModeIOS: true` so it bypasses the
silent switch on iPhones (matching the user-facing intent of "this is a fire
alarm").

## Install on a phone (APK)

For non-technical users who shouldn't have to install Expo Go and scan a QR
code, build a standalone **signed APK** they can install in one tap.

### Build the APK (one command)

Requires a free [Expo account](https://expo.dev/signup). The build runs on
Expo's cloud servers — no local Android SDK or Java needed.

```bash
cd mobile
eas login                                                # one-time
eas build --platform android --profile preview
```

On the first build, EAS offers to generate and store an Android keystore on
Expo's servers — answer **Yes**. It reuses that managed keystore for every
later build, signs the resulting APK, and gives you a download URL when done.
Typical end-to-end time: **10–20 min** (queue + build). Because the keystore is
freshly generated, **uninstall any previously sideloaded build first** — Android
won't install an APK with a different signing key over an existing install.

When the build finishes, download the APK:

```bash
eas build:list --limit 1 --json | jq -r '.[0].artifacts.buildUrl' \
  | xargs curl -L -o build.apk
```

That's the file to send. It will be ~50–60 MB.

### Sanity-check the APK before sending

```bash
ls -lh mobile/build-*.apk
$ANDROID_HOME/build-tools/*/aapt dump badging mobile/build-*.apk | head -3
# expect: package: name='dev.dnachavez.smartlightfirealarm' versionCode='1' versionName='1.0.0'
```

Optionally install to a USB-connected phone with developer mode + USB
debugging enabled:

```bash
adb install mobile/build-*.apk    # expect: Success
```

### Send to the end-user

Send the `.apk` over Google Drive, Gmail, Messenger, or any other file
transfer. Then send them these install steps:

1. Tap the APK in your file manager (or Downloads).
2. Android will say *"For your security, your phone isn't allowed to install
   unknown apps from this source."* Tap **Settings** → toggle **Allow from
   this source** ON → press Back → tap the APK again.
3. Tap **Install** → **Open**.
4. When the app asks for **Notifications**, tap **Allow** (this is how you
   get fire alerts).
5. Tap the **Settings** (gear) icon in the top-right.
6. Enter the Arduino's local IP address (e.g. `192.168.1.42`) and the API
   token your installer gave you.
7. Tap **Test connection** — you should see *"Connected"*.
8. Tap **Save**. The dashboard starts updating every couple of seconds.

That's it — the app now plays a loud siren and sends a phone notification
whenever the Arduino detects flame or smoke. Make sure your phone is on the
**same WiFi network** as the Arduino, or it can't reach it.

### Known limits of the sideloaded APK

- The phone and Arduino must be on the **same WiFi**. There is no cloud
  relay. If WiFi goes down, the app can't reach the Arduino.
- Notifications only fire reliably while the app is in the foreground or
  recently backgrounded. For 24/7 alerting you need a server-side push
  pipeline (out of scope for this MVP). Always pair this app with a
  UL/EN-listed smoke alarm — the phone is not a certified life-safety
  device.
- Android may ask the user to re-enable *"Install unknown apps"* if they
  install through a different file manager than the one they originally
  allowed.
