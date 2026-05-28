import { useEffect, useRef } from 'react';
import { useStatus } from '../state/StatusContext';
import { fireLocalAlertNotification } from './notifications';

const titleFor = (reason: string): string => {
  switch (reason) {
    case 'flame':
      return 'FIRE detected';
    case 'smoke':
      return 'SMOKE detected';
    case 'flame+smoke':
      return 'FIRE + SMOKE detected';
    default:
      return 'Hazard detected';
  }
};

/**
 * Fires exactly one local notification per alertEpisodeId rising edge.
 * The Arduino's monotonic counter is the dedup key — if the user kills
 * the app and reopens it, the same episode never re-notifies because
 * lastNotifiedRef is hydrated from the first observed value.
 */
export const useEpisodeNotifier = () => {
  const { status } = useStatus();
  const lastNotifiedRef = useRef<number | null>(null);

  useEffect(() => {
    if (!status) return;
    const { alertEpisodeId, alertActive, fireDetected, smokeDetected, alarmReason, lastAlertRoom } = status;
    const hazard = alertActive || fireDetected || smokeDetected;
    if (!hazard) return;
    if (alertEpisodeId === 0) return; // no real episode yet
    if (lastNotifiedRef.current === alertEpisodeId) return;
    lastNotifiedRef.current = alertEpisodeId;
    const where = lastAlertRoom != null ? ` in Room ${lastAlertRoom}` : '';
    void fireLocalAlertNotification(
      titleFor(alarmReason),
      `Hazard detected${where}. Check the dashboard.`,
    );
  }, [status]);
};
