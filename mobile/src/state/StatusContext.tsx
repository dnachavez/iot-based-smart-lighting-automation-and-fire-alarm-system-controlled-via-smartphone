import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { AppState, type AppStateStatus } from 'react-native';
import { getStatus, type ApiResult, type ArduinoStatus } from '../api/arduino';
import { useSettings } from './SettingsContext';

export type ConnectionState =
  | { kind: 'idle' }
  | { kind: 'connecting' }
  | { kind: 'connected' }
  | { kind: 'error'; message: string };

type StatusContextValue = {
  status: ArduinoStatus | null;
  lastUpdated: number | null;
  connection: ConnectionState;
  refresh: () => Promise<void>;
};

const StatusContext = createContext<StatusContextValue | null>(null);

const POLL_INTERVAL_MS = 1500;

const describeError = <T,>(res: ApiResult<T>): string => {
  if (res.kind === 'timeout') return 'Timed out reaching Arduino';
  if (res.kind === 'network') return res.message;
  if (res.kind === 'http') return `HTTP ${res.code}`;
  return 'Unknown error';
};

export const StatusProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const { ip } = useSettings();
  const [status, setStatus] = useState<ArduinoStatus | null>(null);
  const [lastUpdated, setLastUpdated] = useState<number | null>(null);
  const [connection, setConnection] = useState<ConnectionState>({ kind: 'idle' });
  const inFlight = useRef(false);
  const foreground = useRef(true);

  const tick = useCallback(async () => {
    if (!ip) {
      setConnection({ kind: 'idle' });
      return;
    }
    if (inFlight.current) return;
    inFlight.current = true;
    if (!status) setConnection({ kind: 'connecting' });
    const res = await getStatus(ip);
    inFlight.current = false;
    if (res.kind === 'ok') {
      setStatus(res.data);
      setLastUpdated(Date.now());
      setConnection({ kind: 'connected' });
    } else {
      setConnection({ kind: 'error', message: describeError(res) });
    }
  }, [ip, status]);

  useEffect(() => {
    let cancelled = false;
    let timer: ReturnType<typeof setInterval> | null = null;

    const start = () => {
      if (timer) return;
      // Fire immediately so the UI updates without waiting one interval
      void tick();
      timer = setInterval(() => {
        if (!cancelled && foreground.current) void tick();
      }, POLL_INTERVAL_MS);
    };

    const stop = () => {
      if (timer) {
        clearInterval(timer);
        timer = null;
      }
    };

    const handleAppState = (next: AppStateStatus) => {
      foreground.current = next === 'active';
      if (next === 'active') start();
      else stop();
    };

    const sub = AppState.addEventListener('change', handleAppState);
    foreground.current = AppState.currentState === 'active';
    if (foreground.current) start();

    return () => {
      cancelled = true;
      stop();
      sub.remove();
    };
  }, [tick]);

  const value = useMemo<StatusContextValue>(
    () => ({ status, lastUpdated, connection, refresh: tick }),
    [status, lastUpdated, connection, tick],
  );

  return <StatusContext.Provider value={value}>{children}</StatusContext.Provider>;
};

export const useStatus = (): StatusContextValue => {
  const ctx = useContext(StatusContext);
  if (!ctx) throw new Error('useStatus must be used inside <StatusProvider>');
  return ctx;
};
