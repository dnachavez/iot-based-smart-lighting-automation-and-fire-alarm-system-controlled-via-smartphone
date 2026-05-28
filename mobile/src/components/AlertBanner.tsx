import React from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { ArduinoStatus } from '../api/arduino';
import { theme } from '../theme';

type Severity = 'safe' | 'clearHold' | 'hazard' | 'latched';

const severityFor = (status: ArduinoStatus | null): Severity => {
  if (!status) return 'safe';
  if (status.fireDetected || status.smokeDetected || status.alarmActive) return 'hazard';
  if (status.episodeActive && status.alarmReason === 'clear_hold') return 'clearHold';
  if (status.alarmReason === 'latched') return 'latched';
  return 'safe';
};

const palette: Record<Severity, { bg: string; fg: string; title: string }> = {
  safe: { bg: '#0c2a1c', fg: '#86efac', title: 'All clear' },
  clearHold: { bg: '#2a230c', fg: '#fde68a', title: 'Hazard cleared — re-arming' },
  hazard: { bg: '#3a0d0d', fg: '#fecaca', title: 'HAZARD DETECTED' },
  latched: { bg: '#2a160c', fg: '#fdba74', title: 'Lockout latched' },
};

const reasonText: Record<string, string> = {
  off: 'No alert',
  commissioning_suppressed: 'Commissioning mode — alerts suppressed',
  flame: 'Flame detected',
  smoke: 'Smoke detected',
  'flame+smoke': 'Flame and smoke detected',
  clear_hold: 'Holding 10 s for clear-hold',
  latched: 'Awaiting manual reset',
};

type Props = { status: ArduinoStatus | null };

export const AlertBanner: React.FC<Props> = ({ status }) => {
  const sev = severityFor(status);
  const { bg, fg, title } = palette[sev];
  const reason = status?.alarmReason ?? 'off';
  const room = status?.lastAlertRoom;
  return (
    <View style={[styles.box, { backgroundColor: bg }]}>
      <Text style={[styles.title, { color: fg }]}>{title}</Text>
      <Text style={[styles.body, { color: fg }]}>
        {reasonText[reason] ?? reason}
        {room != null ? `  •  Last alert: Room ${room}` : ''}
      </Text>
      {status?.commissioningMode && (
        <Text style={[styles.tag, { color: fg }]}>COMMISSIONING MODE</Text>
      )}
    </View>
  );
};

const styles = StyleSheet.create({
  box: {
    padding: 16,
    borderRadius: theme.radius,
    gap: 4,
  },
  title: {
    fontSize: 20,
    fontWeight: '800',
    letterSpacing: 0.3,
  },
  body: {
    fontSize: 14,
    fontWeight: '500',
  },
  tag: {
    marginTop: 6,
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 1,
  },
});
