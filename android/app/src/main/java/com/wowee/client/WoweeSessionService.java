package com.wowee.client;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Keeps the game session open while the client is in the background.
 *
 * Android freezes a backgrounded app's process within seconds, and its socket
 * to the world server goes with it: the server sees the client vanish, and the
 * player comes back to the login screen. A process that holds a foreground
 * service is not frozen. This service does no work of its own - the session
 * lives in the native client, as it always has, and the client keeps running
 * in the background because SDL is told not to block on pause
 * (SDL_HINT_ANDROID_BLOCK_ON_PAUSE, core/window.cpp). It only marks the process
 * as one the player is still using, with the notification Android requires
 * for that.
 *
 * WoweeActivity starts it while it is in the foreground, which is the only time
 * Android allows, and stops it when the activity goes away. DataFolderActivity
 * never starts it: there is no session before the client runs.
 */
public class WoweeSessionService extends Service {

    private static final String CHANNEL_ID = "session";
    private static final int NOTIFICATION_ID = 1;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager != null && manager.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                    getString(R.string.session_channel_name), NotificationManager.IMPORTANCE_LOW);
            channel.setDescription(getString(R.string.session_channel_description));
            channel.setShowBadge(false);
            manager.createNotificationChannel(channel);
        }

        // Tapping the notification brings the running client back. singleTask
        // in the manifest makes this the existing activity, not a second one;
        // NEW_TASK because a service has no task of its own to start it in.
        Intent open = new Intent(this, WoweeActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent tap = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Notification notification = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_session)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(getString(R.string.session_notification_text))
                .setContentIntent(tap)
                .setOngoing(true)
                .setShowWhen(false)
                .build();

        // The type is required from API 34 (Android 14), which is what the app
        // targets; on 33 the two-argument form is the only one there is.
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        // Not restarted on its own: without the client there is no session to
        // keep, and a service brought back after the process died would only
        // hold a notification up over nothing.
        return START_NOT_STICKY;
    }

    /** Swiping the app away ends the session, so it ends this too. */
    @Override
    public void onTaskRemoved(Intent rootIntent) {
        stopSelf();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
