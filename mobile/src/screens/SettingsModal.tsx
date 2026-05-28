import React, { useEffect, useState } from 'react';
import {
  Alert,
  Modal,
  Pressable,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from 'react-native';
import { getHealth } from '../api/arduino';
import { Button } from '../components/Button';
import { useSettings } from '../state/SettingsContext';
import { theme } from '../theme';

type Props = {
  visible: boolean;
  onClose: () => void;
};

export const SettingsModal: React.FC<Props> = ({ visible, onClose }) => {
  const { ip, token, setIp, setToken } = useSettings();
  const [draftIp, setDraftIp] = useState(ip);
  const [draftToken, setDraftToken] = useState(token);
  const [revealToken, setRevealToken] = useState(false);
  const [testing, setTesting] = useState(false);

  useEffect(() => {
    if (visible) {
      setDraftIp(ip);
      setDraftToken(token);
      setRevealToken(false);
    }
  }, [visible, ip, token]);

  const save = () => {
    setIp(draftIp.trim());
    setToken(draftToken);
    onClose();
  };

  const test = async () => {
    if (!draftIp.trim()) {
      Alert.alert('IP required', 'Enter the Arduino IP address first.');
      return;
    }
    setTesting(true);
    const res = await getHealth(draftIp.trim());
    setTesting(false);
    if (res.kind === 'ok') {
      Alert.alert('Connected', `Arduino at ${draftIp.trim()} responded /health OK.`);
    } else if (res.kind === 'timeout') {
      Alert.alert('Timed out', 'No response. Check IP and that the Arduino is on the same WiFi.');
    } else if (res.kind === 'network') {
      Alert.alert('Network error', res.message);
    } else {
      Alert.alert('HTTP error', `Status ${res.code}: ${res.body.slice(0, 200)}`);
    }
  };

  return (
    <Modal visible={visible} animationType="slide" presentationStyle="formSheet" onRequestClose={onClose}>
      <View style={styles.root}>
        <ScrollView contentContainerStyle={styles.content}>
          <View style={styles.headerRow}>
            <Text style={styles.title}>Connection</Text>
            <Pressable onPress={onClose} hitSlop={12}>
              <Text style={styles.close}>Done</Text>
            </Pressable>
          </View>

          <Text style={styles.helper}>
            Both the Arduino and this phone must be on the same WiFi network. Find the
            Arduino's IP in the Arduino IDE Serial Monitor (it prints a line starting with
            "[WIFI] connected ip=").
          </Text>

          <Field label="Arduino IP">
            <TextInput
              value={draftIp}
              onChangeText={setDraftIp}
              autoCapitalize="none"
              autoCorrect={false}
              keyboardType="numbers-and-punctuation"
              placeholder="192.168.1.42"
              placeholderTextColor={theme.textMuted}
              style={styles.input}
            />
          </Field>

          <Field label="API token">
            <TextInput
              value={draftToken}
              onChangeText={setDraftToken}
              autoCapitalize="none"
              autoCorrect={false}
              secureTextEntry={!revealToken}
              placeholder="Shared secret in arduino.ino"
              placeholderTextColor={theme.textMuted}
              style={styles.input}
            />
            <View style={styles.revealRow}>
              <Switch value={revealToken} onValueChange={setRevealToken} />
              <Text style={styles.revealLabel}>Show token</Text>
            </View>
          </Field>

          <View style={styles.actions}>
            <Button label="Test connection" onPress={test} busy={testing} />
            <Button label="Save" onPress={save} tone="primary" />
          </View>
        </ScrollView>
      </View>
    </Modal>
  );
};

const Field: React.FC<{ label: string; children: React.ReactNode }> = ({ label, children }) => (
  <View style={styles.field}>
    <Text style={styles.fieldLabel}>{label}</Text>
    {children}
  </View>
);

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: theme.bg,
  },
  content: {
    padding: 16,
    gap: 16,
  },
  headerRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: {
    color: theme.text,
    fontSize: 22,
    fontWeight: '700',
  },
  close: {
    color: theme.primary,
    fontSize: 16,
    fontWeight: '600',
  },
  helper: {
    color: theme.textMuted,
    fontSize: 13,
    lineHeight: 18,
  },
  field: {
    gap: 8,
  },
  fieldLabel: {
    color: theme.textMuted,
    fontSize: 12,
    fontWeight: '700',
    letterSpacing: 0.8,
  },
  input: {
    backgroundColor: theme.surface,
    borderColor: theme.border,
    borderWidth: 1,
    borderRadius: theme.radius,
    paddingHorizontal: 14,
    paddingVertical: 12,
    color: theme.text,
    fontSize: 16,
  },
  revealRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: 8,
  },
  revealLabel: {
    color: theme.textMuted,
    fontSize: 13,
  },
  actions: {
    flexDirection: 'row',
    gap: 12,
    marginTop: 12,
  },
});
