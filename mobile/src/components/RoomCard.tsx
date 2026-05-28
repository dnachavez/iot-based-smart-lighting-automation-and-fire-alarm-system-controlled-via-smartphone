import React, { useState } from 'react';
import { StyleSheet, Text, View } from 'react-native';
import type { RoomStatus } from '../api/arduino';
import { setLight } from '../api/arduino';
import { useSettings } from '../state/SettingsContext';
import { useStatus } from '../state/StatusContext';
import { theme } from '../theme';
import { Button } from './Button';

type Props = { room: RoomStatus };

export const RoomCard: React.FC<Props> = ({ room }) => {
  const { ip, token } = useSettings();
  const { refresh } = useStatus();
  const [busy, setBusy] = useState(false);

  const flame = room.flameSensorEnabled && room.flameDetected;
  const smoke = room.smokeSensorEnabled && room.smokeDetected;
  const lockout = room.fireLockout;

  const toggle = async () => {
    if (!ip || !token) return;
    setBusy(true);
    await setLight(ip, token, room.room as 1 | 2 | 3 | 4 | 5, room.lightOn ? 'off' : 'on');
    await refresh();
    setBusy(false);
  };

  return (
    <View
      style={[
        styles.card,
        lockout && styles.lockoutBorder,
        room.lightOn && !lockout && styles.activeBorder,
      ]}
    >
      <View style={styles.headerRow}>
        <Text style={styles.title}>Room {room.room}</Text>
        <View
          style={[
            styles.pill,
            { backgroundColor: room.lightOn ? theme.ok : theme.surfaceAlt },
          ]}
        >
          <Text
            style={[
              styles.pillText,
              { color: room.lightOn ? '#04240f' : theme.textMuted },
            ]}
          >
            {room.lightOn ? 'LIGHT ON' : 'LIGHT OFF'}
          </Text>
        </View>
      </View>

      <View style={styles.statsRow}>
        <Stat
          label="Flame"
          value={flame ? 'YES' : 'no'}
          danger={flame}
          disabled={!room.flameSensorEnabled}
        />
        <Stat
          label="Smoke"
          value={smoke ? 'YES' : 'no'}
          danger={smoke}
          disabled={!room.smokeSensorEnabled}
        />
        <Stat label="MQ-2" value={`${room.smokeValue}`} sub={`thr ${room.smokeThreshold}`} />
      </View>

      {lockout ? (
        <View style={styles.lockoutBox}>
          <Text style={styles.lockoutText}>Locked — reset alert when safe</Text>
        </View>
      ) : (
        <Button
          label={room.lightOn ? 'Turn light off' : 'Turn light on'}
          onPress={toggle}
          tone={room.lightOn ? 'neutral' : 'primary'}
          disabled={!ip || !token}
          busy={busy}
        />
      )}
    </View>
  );
};

type StatProps = {
  label: string;
  value: string;
  sub?: string;
  danger?: boolean;
  disabled?: boolean;
};

const Stat: React.FC<StatProps> = ({ label, value, sub, danger, disabled }) => (
  <View style={styles.stat}>
    <Text style={styles.statLabel}>{label}</Text>
    <Text
      style={[
        styles.statValue,
        danger && styles.statDanger,
        disabled && styles.statDisabled,
      ]}
    >
      {disabled ? 'off' : value}
    </Text>
    {sub ? <Text style={styles.statSub}>{sub}</Text> : null}
  </View>
);

const styles = StyleSheet.create({
  card: {
    backgroundColor: theme.surface,
    borderRadius: theme.radius,
    padding: 16,
    borderWidth: 1,
    borderColor: theme.border,
    gap: 12,
  },
  lockoutBorder: {
    borderColor: theme.danger,
  },
  activeBorder: {
    borderColor: theme.ok,
  },
  headerRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: {
    color: theme.text,
    fontSize: 18,
    fontWeight: '700',
  },
  pill: {
    paddingHorizontal: 10,
    paddingVertical: 4,
    borderRadius: 999,
  },
  pillText: {
    fontSize: 11,
    fontWeight: '700',
    letterSpacing: 0.8,
  },
  statsRow: {
    flexDirection: 'row',
    gap: 16,
  },
  stat: {
    flex: 1,
  },
  statLabel: {
    color: theme.textMuted,
    fontSize: 11,
    letterSpacing: 0.8,
    fontWeight: '700',
  },
  statValue: {
    color: theme.text,
    fontSize: 18,
    fontWeight: '700',
  },
  statSub: {
    color: theme.textMuted,
    fontSize: 11,
  },
  statDanger: {
    color: theme.danger,
  },
  statDisabled: {
    color: theme.textMuted,
    fontStyle: 'italic',
  },
  lockoutBox: {
    backgroundColor: '#3a0d0d',
    padding: 12,
    borderRadius: theme.radius,
  },
  lockoutText: {
    color: '#fecaca',
    fontSize: 13,
    fontWeight: '600',
    textAlign: 'center',
  },
});
