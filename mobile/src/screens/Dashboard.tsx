import React, { useState } from 'react';
import {
  Alert,
  RefreshControl,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';
import { allLightsOff, resetAlert } from '../api/arduino';
import { AlertBanner } from '../components/AlertBanner';
import { Button } from '../components/Button';
import { RoomCard } from '../components/RoomCard';
import { useSettings } from '../state/SettingsContext';
import { useStatus } from '../state/StatusContext';
import { theme } from '../theme';
import { useAlarmSiren } from '../alert/useAlarmSiren';
import { useEpisodeNotifier } from '../alert/useEpisodeNotifier';
import { SettingsModal } from './SettingsModal';

const formatAgo = (ms: number | null): string => {
  if (ms == null) return '—';
  const delta = Math.max(0, Math.floor((Date.now() - ms) / 1000));
  if (delta < 2) return 'just now';
  if (delta < 60) return `${delta}s ago`;
  return `${Math.floor(delta / 60)}m ago`;
};

export const Dashboard: React.FC = () => {
  const { ip, token } = useSettings();
  const { status, lastUpdated, connection, refresh } = useStatus();
  const { hazard, muted, mute, unmute, testSound } = useAlarmSiren();
  useEpisodeNotifier();

  const [settingsVisible, setSettingsVisible] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [busy, setBusy] = useState<null | 'allOff' | 'reset'>(null);

  const onRefresh = async () => {
    setRefreshing(true);
    await refresh();
    setRefreshing(false);
  };

  const handleAllOff = async () => {
    if (!ip || !token) return;
    setBusy('allOff');
    const res = await allLightsOff(ip, token);
    setBusy(null);
    if (res.kind !== 'ok') {
      Alert.alert('All-lights-off failed', describeError(res));
    } else {
      await refresh();
    }
  };

  const handleReset = async () => {
    if (!ip || !token) return;
    setBusy('reset');
    const res = await resetAlert(ip, token);
    setBusy(null);
    if (res.kind === 'ok') {
      if ('reset' in res.data && res.data.reset) {
        Alert.alert('Reset', 'Alert cleared.');
      } else if ('blockingRooms' in res.data) {
        const rooms = res.data.blockingRooms
          .map((r) => `Room ${r.room} (${r.reason})`)
          .join(', ');
        Alert.alert('Cannot reset yet', `Hazard still active in: ${rooms || 'unknown'}`);
      }
      await refresh();
    } else if (res.kind === 'http' && res.code === 409) {
      Alert.alert('Cannot reset yet', 'A hazard is still detected. Wait for it to clear.');
      await refresh();
    } else {
      Alert.alert('Reset failed', describeError(res));
    }
  };

  const needsSetup = !ip || !token;

  return (
    <View style={styles.root}>
      <ScrollView
        contentContainerStyle={styles.scroll}
        refreshControl={<RefreshControl tintColor={theme.text} refreshing={refreshing} onRefresh={onRefresh} />}
      >
        <View style={styles.headerBar}>
          <View style={styles.headerLeft}>
            <View
              style={[
                styles.dot,
                {
                  backgroundColor:
                    connection.kind === 'connected'
                      ? theme.ok
                      : connection.kind === 'connecting'
                        ? theme.warn
                        : connection.kind === 'error'
                          ? theme.danger
                          : theme.textMuted,
                },
              ]}
            />
            <Text style={styles.headerTitle}>Fire Alarm</Text>
          </View>
          <Text style={styles.headerSub} numberOfLines={1}>
            {connection.kind === 'connected'
              ? `${ip}  •  ${formatAgo(lastUpdated)}`
              : connection.kind === 'error'
                ? connection.message
                : connection.kind === 'connecting'
                  ? 'Connecting…'
                  : 'Not configured'}
          </Text>
        </View>

        {needsSetup && (
          <View style={styles.setupCard}>
            <Text style={styles.setupText}>
              Open Settings and enter the Arduino's IP address and API token to begin.
            </Text>
            <Button label="Open settings" tone="primary" onPress={() => setSettingsVisible(true)} />
          </View>
        )}

        <AlertBanner status={status} />

        <View style={styles.controls}>
          <Button
            label="All lights off"
            tone="neutral"
            onPress={handleAllOff}
            busy={busy === 'allOff'}
            disabled={needsSetup}
          />
          <Button
            label="Reset alert"
            tone="warn"
            onPress={handleReset}
            busy={busy === 'reset'}
            disabled={needsSetup}
          />
          <Button
            label={muted ? 'Unmute alarm' : 'Mute alarm'}
            tone={muted ? 'ok' : 'neutral'}
            onPress={() => (muted ? unmute() : mute())}
            disabled={!hazard && !muted}
          />
          <Button label="Test alarm" tone="neutral" onPress={testSound} />
          <Button label="Refresh" tone="neutral" onPress={onRefresh} busy={refreshing} />
          <Button label="Settings" tone="neutral" onPress={() => setSettingsVisible(true)} />
        </View>

        {status?.rooms?.map((r) => (
          <RoomCard key={r.room} room={r} />
        ))}

        {status && (
          <View style={styles.footer}>
            <Text style={styles.footerLine}>Uptime: {Math.floor(status.uptimeMs / 1000)} s</Text>
            <Text style={styles.footerLine}>
              Episode #{status.alertEpisodeId} • {status.alarmReason}
            </Text>
            <Text style={styles.footerLine}>
              WiFi: {status.wifi.connected ? `${status.wifi.ip} (${status.wifi.rssi} dBm)` : 'disconnected'}
            </Text>
          </View>
        )}
      </ScrollView>

      <SettingsModal visible={settingsVisible} onClose={() => setSettingsVisible(false)} />
    </View>
  );
};

const describeError = (res: { kind: string; message?: string; code?: number; body?: string }): string => {
  if (res.kind === 'timeout') return 'Timed out.';
  if (res.kind === 'network') return res.message ?? 'Network error.';
  if (res.kind === 'http') return `HTTP ${res.code}: ${(res.body ?? '').slice(0, 120)}`;
  return 'Unknown error.';
};

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: theme.bg,
  },
  scroll: {
    padding: 16,
    paddingBottom: 48,
    gap: 14,
  },
  headerBar: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    gap: 12,
  },
  headerLeft: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 10,
  },
  headerTitle: {
    color: theme.text,
    fontSize: 22,
    fontWeight: '800',
  },
  headerSub: {
    flex: 1,
    color: theme.textMuted,
    fontSize: 12,
    textAlign: 'right',
  },
  dot: {
    width: 10,
    height: 10,
    borderRadius: 5,
  },
  setupCard: {
    backgroundColor: theme.surface,
    borderColor: theme.border,
    borderWidth: 1,
    borderRadius: theme.radius,
    padding: 16,
    gap: 12,
  },
  setupText: {
    color: theme.text,
    fontSize: 14,
  },
  controls: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 10,
  },
  footer: {
    marginTop: 12,
    padding: 12,
    backgroundColor: theme.surface,
    borderRadius: theme.radius,
    borderColor: theme.border,
    borderWidth: 1,
    gap: 4,
  },
  footerLine: {
    color: theme.textMuted,
    fontSize: 12,
    fontVariant: ['tabular-nums'],
  },
});
