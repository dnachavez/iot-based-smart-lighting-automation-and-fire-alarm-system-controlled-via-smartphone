// Typed HTTP client for the Arduino Uno R4 firmware. All calls are AbortController-bounded.
// The Arduino is on the LAN; we never resolve cloud hostnames here.

export type RoomStatus = {
  room: number;
  lightOn: boolean;
  flameSensorEnabled: boolean;
  smokeSensorEnabled: boolean;
  flameRaw: 'H' | 'L';
  flameRawLevel: 'H' | 'L';
  flameSafeLevel: 'H' | 'L';
  flameDetectedLevel: 'H' | 'L';
  flameDetected: boolean;
  fireLockout: boolean;
  smokeValue: number;
  smokeRaw: number;
  smokeThreshold: number;
  smokeBaseline: number | null;
  smokeDelta: number | null;
  smokeDetected: boolean;
};

export type AlarmReason =
  | 'off'
  | 'commissioning_suppressed'
  | 'flame'
  | 'smoke'
  | 'flame+smoke'
  | 'clear_hold'
  | 'latched';

export type ArduinoStatus = {
  wifi: { connected: boolean; ip: string; rssi: number };
  uptimeMs: number;
  commissioningMode: boolean;
  wouldAlert: boolean;
  actualAlert: boolean;
  alertActive: boolean;
  alarmActive: boolean;
  episodeActive: boolean;
  episodeTimedOut: boolean;
  rearmed: boolean;
  clearHoldRemainingMs: number;
  autoRestorePending: boolean;
  restoreMode: string;
  alarmReason: AlarmReason;
  alertEpisodeId: number;
  fireDetected: boolean;
  smokeDetected: boolean;
  rearmClearHoldMs: number;
  autoRestoreOnSafe: boolean;
  flameCalibrationComplete: boolean;
  smokeBaselineReady: boolean;
  lastAlertRoom: number | null;
  rooms: RoomStatus[];
};

export type ResetAlertResponse =
  | { ok: true; reset: true }
  | { ok: true; message: string }
  | {
      ok: false;
      message: string;
      blockingRooms: { room: number; reason: 'flame' | 'smoke' }[];
    };

export type ApiResult<T> =
  | { kind: 'ok'; data: T }
  | { kind: 'timeout' }
  | { kind: 'network'; message: string }
  | { kind: 'http'; code: number; body: string };

const DEFAULT_TIMEOUT_MS = 2500;

const fetchJson = async <T>(
  url: string,
  init: RequestInit = {},
  timeoutMs = DEFAULT_TIMEOUT_MS,
): Promise<ApiResult<T>> => {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, { ...init, signal: controller.signal });
    const text = await res.text();
    if (!res.ok) {
      return { kind: 'http', code: res.status, body: text };
    }
    try {
      return { kind: 'ok', data: JSON.parse(text) as T };
    } catch {
      return { kind: 'network', message: 'Invalid JSON in response' };
    }
  } catch (err: unknown) {
    if (err instanceof Error && err.name === 'AbortError') {
      return { kind: 'timeout' };
    }
    return {
      kind: 'network',
      message: err instanceof Error ? err.message : 'Network error',
    };
  } finally {
    clearTimeout(timer);
  }
};

const baseUrl = (ip: string) => `http://${ip.trim()}`;

export const getHealth = (ip: string) =>
  fetchJson<{ ok: true }>(`${baseUrl(ip)}/health`);

export const getStatus = (ip: string, timeoutMs?: number) =>
  fetchJson<ArduinoStatus>(`${baseUrl(ip)}/status`, {}, timeoutMs);

export const setLight = (
  ip: string,
  token: string,
  room: 1 | 2 | 3 | 4 | 5,
  state: 'on' | 'off',
) =>
  fetchJson<{ ok: true; room: number; light: boolean }>(
    `${baseUrl(ip)}/light?room=${room}&state=${state}&token=${encodeURIComponent(token)}`,
    { method: 'POST' },
  );

export const allLightsOff = (ip: string, token: string) =>
  fetchJson<{ ok: true; allOff: true }>(
    `${baseUrl(ip)}/all-lights?state=off&token=${encodeURIComponent(token)}`,
    { method: 'POST' },
  );

export const resetAlert = (ip: string, token: string) =>
  fetchJson<ResetAlertResponse>(
    `${baseUrl(ip)}/reset-alert?token=${encodeURIComponent(token)}`,
    { method: 'POST' },
    4000,
  );
