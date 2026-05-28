import { StatusBar } from 'expo-status-bar';
import React, { useEffect } from 'react';
import { SafeAreaView, StyleSheet } from 'react-native';
import { setupNotifications } from './src/alert/notifications';
import { Dashboard } from './src/screens/Dashboard';
import { SettingsProvider } from './src/state/SettingsContext';
import { StatusProvider } from './src/state/StatusContext';
import { theme } from './src/theme';

export default function App() {
  useEffect(() => {
    void setupNotifications();
  }, []);

  return (
    <SettingsProvider>
      <StatusProvider>
        <SafeAreaView style={styles.root}>
          <StatusBar style="light" />
          <Dashboard />
        </SafeAreaView>
      </StatusProvider>
    </SettingsProvider>
  );
}

const styles = StyleSheet.create({
  root: {
    flex: 1,
    backgroundColor: theme.bg,
  },
});
