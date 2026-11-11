package org.openmt.radio.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.text.DateFormat;
import java.util.Date;
import java.util.Map;

import org.openmt.radio.core.Json;
import org.openmt.radio.core.RadioClient;

/** Keeps the connection to the radio and the audio going while the screen is off or another app is in front (a foreground service
 *  with a notification, as Android requires for that).  The screen (MainActivity) binds to it and shows what it reports. */
public final class RadioService extends Service implements RadioClient.Listener
{
    static final String ACTION_START = "org.openmt.radio.START";
    static final String ACTION_STOP = "org.openmt.radio.STOP";
    private static final String CHANNEL = "openmt_radio";
    private static final int NOTE_ID = 1;

    /** the screen's side: called from any thread; post to the main thread */
    interface Ui
    {
        void changed();
    }

    final class LocalBinder extends Binder
    {
        RadioService service()
        {
            return RadioService.this;
        }
    }

    private final IBinder binder = new LocalBinder();
    private RadioClient client;
    private AndroidAudio audio;
    private PowerManager.WakeLock wake;
    private Prefs prefs;
    private volatile Ui ui;

    /* what the screen shows */
    private final Object lock = new Object();
    private String link = "offline";
    private String linkMessage = "";
    private Map<String, Object> state;
    private String callText = "";
    private String pttMessage = "";
    private boolean holding;
    private Map<String, Object> monitor;
    private final StringBuilder log = new StringBuilder();

    @Override
    public void onCreate()
    {
        super.onCreate();
        prefs = new Prefs(this);
        audio = new AndroidAudio();
        client = new RadioClient(this, audio, audio);
        audio.setClient(client);
        audio.setVolume(prefs.volume() / 100.0F);
        audio.setMicLevel(prefs.micLevel() / 100.0F);

        NotificationManager nm = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel(CHANNEL, "OpenMT Radio", NotificationManager.IMPORTANCE_LOW));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        String action = intent == null ? ACTION_START : intent.getAction();

        if (ACTION_STOP.equals(action))
        {
            prefs.setBool("linkOff", true);          /* the app stays disconnected until the person connects again */
            client.stop();
            releaseWake();
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }

        startInForeground("Connecting ...");
        if (!prefs.paired())
        {
            stopForeground(true);
            stopSelf();
            return START_NOT_STICKY;
        }
        acquireWake();
        client.setTarget(prefs.host(), prefs.port(), prefs.token(), prefs.fingerprint());
        client.start();
        return START_REDELIVER_INTENT;
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return binder;
    }

    @Override
    public void onDestroy()
    {
        client.stop();
        audio.release();
        releaseWake();
        super.onDestroy();
    }

    private void acquireWake()
    {
        if (wake == null)
        {
            PowerManager pm = (PowerManager)getSystemService(Context.POWER_SERVICE);
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "openmt:radio");
            wake.setReferenceCounted(false);
        }
        if (!wake.isHeld())
            wake.acquire();
    }

    private void releaseWake()
    {
        if (wake != null && wake.isHeld())
            wake.release();
    }

    /* ------------------------------------------------------------------------------------------------ notification */

    private void startInForeground(String text)
    {
        Notification n = notification(text);

        if (Build.VERSION.SDK_INT >= 29)
        {
            int type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
            /* the microphone type is only allowed once the person has granted the microphone */
            if (checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED)
                type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
            startForeground(NOTE_ID, n, type);
        }
        else
            startForeground(NOTE_ID, n);
    }

    private Notification notification(String text)
    {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent openPi = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        Intent stop = new Intent(this, RadioService.class).setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(this, 1, stop, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        return new Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentTitle("OpenMT Radio")
            .setContentText(text)
            .setContentIntent(openPi)
            .setOngoing(true)
            .addAction(new Notification.Action.Builder(android.R.drawable.ic_menu_close_clear_cancel, "Disconnect", stopPi).build())
            .build();
    }

    private void updateNotification()
    {
        String text;
        synchronized (lock)
        {
            if (!"online".equals(link))
                text = link.substring(0, 1).toUpperCase() + link.substring(1) + (linkMessage.length() > 0 ? ": " + linkMessage : "");
            else if (state != null)
                text = Json.str(state, "modeLabel", "") + " - " + Json.str(state, "status", "");
            else
                text = "Online";
        }
        NotificationManager nm = (NotificationManager)getSystemService(Context.NOTIFICATION_SERVICE);
        nm.notify(NOTE_ID, notification(text));
    }

    /* ------------------------------------------------------------------------------------------------ for the screen */

    RadioClient client()
    {
        return client;
    }

    AndroidAudio audio()
    {
        return audio;
    }

    void setUi(Ui u)
    {
        ui = u;
    }

    String link()
    {
        synchronized (lock)
        {
            return link;
        }
    }

    String linkMessage()
    {
        synchronized (lock)
        {
            return linkMessage;
        }
    }

    Map<String, Object> state()
    {
        synchronized (lock)
        {
            return state;
        }
    }

    String callText()
    {
        synchronized (lock)
        {
            return callText;
        }
    }

    String pttMessage()
    {
        synchronized (lock)
        {
            return pttMessage;
        }
    }

    boolean holding()
    {
        synchronized (lock)
        {
            return holding;
        }
    }

    Map<String, Object> monitor()
    {
        synchronized (lock)
        {
            return monitor;
        }
    }

    String logText()
    {
        synchronized (lock)
        {
            return log.toString();
        }
    }

    private void changed()
    {
        Ui u = ui;
        if (u != null)
            u.changed();
    }

    /* ------------------------------------------------------------------------------------------------ RadioClient.Listener */

    public void onLink(String s, String message)
    {
        synchronized (lock)
        {
            link = s;
            linkMessage = message == null ? "" : message;
            if (!"online".equals(s))
                state = null;
        }
        updateNotification();
        changed();
    }

    public void onState(Map<String, Object> s)
    {
        synchronized (lock)
        {
            state = s;
        }
        updateNotification();
        changed();
    }

    public void onCall(Map<String, Object> e)
    {
        String ev = Json.str(e, "event", "");
        String what = ev.startsWith("tx") ? "Transmitting" : ev.equals("rx_end") ? "Received" : ev.equals("rx_lost") ? "Lost" : "Receiving";
        String text = what + ": " + (Json.str(e, "text", "").length() > 0 ? Json.str(e, "text", "") + "  " : "")
                    + "src " + Json.str(e, "src", "?") + " > dst " + Json.str(e, "dst", "?") + (Json.bool(e, "group", false) ? " (group)" : "");
        double ber = Json.num(e, "ber", 0.0);
        if (ber > 0.0)
            text += String.format("  BER %.1f%%", ber);
        synchronized (lock)
        {
            callText = text;
        }
        changed();
    }

    public void onLog(String text, long timeMs)
    {
        synchronized (lock)
        {
            log.append(DateFormat.getTimeInstance().format(new Date(timeMs))).append("  ").append(text).append('\n');
            if (log.length() > 60000)
                log.delete(0, log.length() - 40000);
        }
        changed();
    }

    public void onPtt(boolean h, String message)
    {
        synchronized (lock)
        {
            holding = h;
            pttMessage = message == null ? "" : message;
        }
        changed();
    }

    public void onMonitor(Map<String, Object> m)
    {
        synchronized (lock)
        {
            monitor = m;
        }
        changed();
    }
}
