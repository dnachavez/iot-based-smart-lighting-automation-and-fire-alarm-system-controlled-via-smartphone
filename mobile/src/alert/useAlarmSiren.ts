import { Audio } from 'expo-av';
import { useEffect, useRef } from 'react';
import { useSettings } from '../state/SettingsContext';
import { useStatus } from '../state/StatusContext';
import { ALARM_WAV_DATA_URI } from './generateAlarmSound';

let cachedSound: Audio.Sound | null = null;
let cachedReady: Promise<Audio.Sound> | null = null;
let lastMutedEpisodeId: number | null = null;

const ensureSound = async (): Promise<Audio.Sound> => {
  if (cachedSound) return cachedSound;
  if (cachedReady) return cachedReady;
  cachedReady = (async () => {
    await Audio.setAudioModeAsync({
      allowsRecordingIOS: false,
      playsInSilentModeIOS: true,
      shouldDuckAndroid: false,
      staysActiveInBackground: false,
    });
    const { sound } = await Audio.Sound.createAsync(
      { uri: ALARM_WAV_DATA_URI },
      { shouldPlay: false, isLooping: true, volume: 1.0 },
    );
    cachedSound = sound;
    return sound;
  })();
  return cachedReady;
};

const playSiren = async () => {
  try {
    const sound = await ensureSound();
    const state = await sound.getStatusAsync();
    if (state.isLoaded && !state.isPlaying) {
      await sound.setPositionAsync(0);
      await sound.playAsync();
    }
  } catch {
    // device may not have audio permissions; visual alert still applies
  }
};

const stopSiren = async () => {
  if (!cachedSound) return;
  try {
    const state = await cachedSound.getStatusAsync();
    if (state.isLoaded && state.isPlaying) {
      await cachedSound.stopAsync();
    }
  } catch {
    // ignore
  }
};

/**
 * Drives the looping in-app siren based on hazard state and the local mute flag.
 *
 * Mute semantics: tapping "Mute" silences the siren for the current
 * alertEpisodeId only. A brand-new episode (counter increments) auto-unmutes
 * so a fresh hazard always sounds the phone, even after the user muted the
 * previous one.
 */
export const useAlarmSiren = () => {
  const { status } = useStatus();
  const { muted, setMuted } = useSettings();
  const lastEpisodeRef = useRef<number | null>(null);

  // Auto-unmute on new episode
  useEffect(() => {
    const episodeId = status?.alertEpisodeId ?? null;
    if (episodeId == null) return;
    if (lastEpisodeRef.current != null && episodeId !== lastEpisodeRef.current) {
      if (muted && lastMutedEpisodeId !== episodeId) {
        setMuted(false);
      }
    }
    lastEpisodeRef.current = episodeId;
  }, [status?.alertEpisodeId, muted, setMuted]);

  const hazard = Boolean(
    status &&
      (status.alertActive || status.fireDetected || status.smokeDetected),
  );

  useEffect(() => {
    if (hazard && !muted) {
      void playSiren();
    } else {
      void stopSiren();
    }
  }, [hazard, muted]);

  useEffect(() => {
    return () => {
      // Don't unload the cached sound between unmounts — the provider stays
      // mounted for app lifetime and re-loading the WAV is expensive.
      void stopSiren();
    };
  }, []);

  return {
    hazard,
    muted,
    mute: () => {
      lastMutedEpisodeId = status?.alertEpisodeId ?? null;
      setMuted(true);
    },
    unmute: () => {
      lastMutedEpisodeId = null;
      setMuted(false);
    },
    testSound: async () => {
      const sound = await ensureSound();
      try {
        const state = await sound.getStatusAsync();
        if (state.isLoaded) {
          await sound.setPositionAsync(0);
          await sound.playAsync();
          setTimeout(() => {
            void sound.stopAsync().catch(() => {});
          }, 5000);
        }
      } catch {
        // ignore
      }
    },
  };
};
