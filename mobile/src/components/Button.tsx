import React from 'react';
import {
  ActivityIndicator,
  Pressable,
  StyleSheet,
  Text,
  type ViewStyle,
} from 'react-native';
import { theme } from '../theme';

type Tone = 'primary' | 'neutral' | 'danger' | 'warn' | 'ok';

const toneBg: Record<Tone, string> = {
  primary: theme.primary,
  neutral: theme.surfaceAlt,
  danger: theme.danger,
  warn: theme.warn,
  ok: theme.ok,
};

const toneFg: Record<Tone, string> = {
  primary: theme.primaryText,
  neutral: theme.text,
  danger: '#ffffff',
  warn: '#1a1300',
  ok: '#04240f',
};

type Props = {
  label: string;
  onPress: () => void;
  tone?: Tone;
  disabled?: boolean;
  busy?: boolean;
  compact?: boolean;
  style?: ViewStyle;
};

export const Button: React.FC<Props> = ({
  label,
  onPress,
  tone = 'neutral',
  disabled,
  busy,
  compact,
  style,
}) => {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled || busy}
      style={({ pressed }) => [
        styles.base,
        compact && styles.compact,
        { backgroundColor: toneBg[tone], opacity: disabled ? 0.5 : pressed ? 0.85 : 1 },
        style,
      ]}
    >
      {busy ? (
        <ActivityIndicator color={toneFg[tone]} />
      ) : (
        <Text style={[styles.label, { color: toneFg[tone] }]}>{label}</Text>
      )}
    </Pressable>
  );
};

const styles = StyleSheet.create({
  base: {
    minHeight: 48,
    paddingHorizontal: 18,
    borderRadius: theme.radius,
    alignItems: 'center',
    justifyContent: 'center',
  },
  compact: {
    minHeight: 36,
    paddingHorizontal: 12,
  },
  label: {
    fontSize: 15,
    fontWeight: '600',
    letterSpacing: 0.2,
  },
});
