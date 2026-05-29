import AsyncStorage from '@react-native-async-storage/async-storage';
import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
} from 'react';

type Settings = {
  ip: string;
  token: string;
  muted: boolean;
};

const STORAGE_KEY = 'firealarm.settings.v1';

const DEFAULTS: Settings = {
  ip: '',
  token: '',
  muted: false,
};

type SettingsContextValue = Settings & {
  ready: boolean;
  setIp: (ip: string) => void;
  setToken: (token: string) => void;
  setMuted: (muted: boolean) => void;
};

const SettingsContext = createContext<SettingsContextValue | null>(null);

export const SettingsProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [settings, setSettings] = useState<Settings>(DEFAULTS);
  const [ready, setReady] = useState(false);

  useEffect(() => {
    (async () => {
      try {
        const raw = await AsyncStorage.getItem(STORAGE_KEY);
        if (raw) {
          const parsed = JSON.parse(raw) as Partial<Settings>;
          setSettings({
            ip: typeof parsed.ip === 'string' ? parsed.ip : '',
            token: typeof parsed.token === 'string' ? parsed.token : '',
            muted: false, // mute is intentionally not persisted across launches
          });
        }
      } catch {
        // ignore — fall back to defaults
      } finally {
        setReady(true);
      }
    })();
  }, []);

  // Merge a partial change against the *latest* state via a functional update.
  // Passing the patch (not a pre-merged object) means back-to-back setIp/setToken
  // calls in one handler don't clobber each other with a stale snapshot.
  const persist = useCallback((patch: Partial<Settings>) => {
    setSettings((prev) => {
      const next = { ...prev, ...patch };
      AsyncStorage.setItem(
        STORAGE_KEY,
        JSON.stringify({ ip: next.ip, token: next.token }),
      ).catch(() => {
        // swallow — settings will revert to disk on next launch
      });
      return next;
    });
  }, []);

  const value = useMemo<SettingsContextValue>(
    () => ({
      ...settings,
      ready,
      setIp: (ip) => persist({ ip }),
      setToken: (token) => persist({ token }),
      setMuted: (muted) => setSettings((s) => ({ ...s, muted })),
    }),
    [settings, ready, persist],
  );

  return <SettingsContext.Provider value={value}>{children}</SettingsContext.Provider>;
};

export const useSettings = (): SettingsContextValue => {
  const ctx = useContext(SettingsContext);
  if (!ctx) throw new Error('useSettings must be used inside <SettingsProvider>');
  return ctx;
};
