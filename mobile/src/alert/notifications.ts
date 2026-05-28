import * as Notifications from 'expo-notifications';
import { Platform } from 'react-native';

export const FIRE_CHANNEL_ID = 'fire-alarm';

let configured = false;

// Foreground policy: surface the banner + sound even when the app is open
// so users see the alert even if they're already poking around the dashboard.
Notifications.setNotificationHandler({
  handleNotification: async () => ({
    shouldShowAlert: true,
    shouldPlaySound: true,
    shouldSetBadge: false,
    shouldShowBanner: true,
    shouldShowList: true,
  }),
});

export const setupNotifications = async (): Promise<boolean> => {
  if (configured) return true;
  configured = true;

  if (Platform.OS === 'android') {
    await Notifications.setNotificationChannelAsync(FIRE_CHANNEL_ID, {
      name: 'Fire alarm',
      importance: Notifications.AndroidImportance.MAX,
      vibrationPattern: [0, 500, 200, 500],
      lightColor: '#ef4444',
      lockscreenVisibility: Notifications.AndroidNotificationVisibility.PUBLIC,
      sound: 'default',
      bypassDnd: true,
    });
  }

  const existing = await Notifications.getPermissionsAsync();
  if (existing.granted) return true;
  if (existing.canAskAgain === false) return false;
  const requested = await Notifications.requestPermissionsAsync();
  return requested.granted;
};

export const fireLocalAlertNotification = async (
  title: string,
  body: string,
): Promise<void> => {
  await Notifications.scheduleNotificationAsync({
    content: {
      title,
      body,
      sound: 'default',
      priority: Notifications.AndroidNotificationPriority.MAX,
      vibrate: [0, 500, 200, 500],
    },
    trigger:
      Platform.OS === 'android' ? { channelId: FIRE_CHANNEL_ID } as Notifications.NotificationTriggerInput : null,
  });
};
