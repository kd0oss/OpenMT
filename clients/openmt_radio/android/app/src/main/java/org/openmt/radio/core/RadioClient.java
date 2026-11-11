package org.openmt.radio.core;

import java.io.IOException;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;

/** Everything the app does with a radio, without Android: keeps the connection (reconnecting by itself), starts the audio, holds the
 *  state, runs commands off the caller's thread, and looks after the push to talk (keepalives, releasing it when anything goes
 *  wrong).  The Android code gives it a Speaker and a Microphone and shows what the Listener reports. */
public final class RadioClient
{
    public interface Listener
    {
        /** "connecting", "online", "reconnecting", "offline", "failed" (the message says why; no more attempts) */
        void onLink(String state, String message);

        void onState(Map<String, Object> state);

        void onCall(Map<String, Object> event);

        void onLog(String text, long timeMs);

        /** whether this client holds the transmitter now, and why not when it was refused or taken away */
        void onPtt(boolean holding, String message);

        void onMonitor(Map<String, Object> monitor);
    }

    public interface Speaker
    {
        void play(short[] pcm8k, int count);

        void clear();
    }

    public interface Microphone
    {
        /** record and call RadioClient.feedMic() every 20 ms until stop() */
        void start();

        void stop();
    }

    public interface Result
    {
        void done(boolean ok, Map<String, Object> result, String error);
    }

    private final Listener listener;
    private final Speaker speaker;
    private final Microphone microphone;

    private volatile String host = "";
    private volatile int port = 18060;
    private volatile String token = "";
    private volatile String pinned = "";

    private volatile boolean running;
    private volatile RemoteConnection conn;
    private Thread loop;
    private volatile Map<String, Object> lastState;

    private volatile boolean audioOn = true;
    private volatile boolean ulaw = true;
    private volatile boolean monitorOn;

    private volatile boolean wantHold;
    private volatile boolean holding;
    private Thread keepalive;
    private final Object pttLock = new Object();

    private final ExecutorService exec = Executors.newSingleThreadExecutor(new ThreadFactory()
    {
        public Thread newThread(Runnable r)
        {
            Thread t = new Thread(r, "openmt-commands");
            t.setDaemon(true);
            return t;
        }
    });

    public int reconnectMaxMs = 15000;

    public RadioClient(Listener listener, Speaker speaker, Microphone microphone)
    {
        this.listener = listener;
        this.speaker = speaker;
        this.microphone = microphone;
    }

    public void setTarget(String host, int port, String token, String pinnedFingerprint)
    {
        this.host = host;
        this.port = port;
        this.token = token;
        this.pinned = pinnedFingerprint == null ? "" : pinnedFingerprint;
    }

    public Map<String, Object> state()
    {
        return lastState;
    }

    public boolean isOnline()
    {
        RemoteConnection c = conn;
        return c != null && c.isOpen();
    }

    public boolean isHolding()
    {
        return holding;
    }

    /* ---------------------------------------------------------------------------------------------- connection */

    public synchronized void start()
    {
        if (running)
            return;
        running = true;
        loop = new Thread(new Runnable()
        {
            public void run()
            {
                runLoop();
            }
        }, "openmt-link");
        loop.setDaemon(true);
        loop.start();
    }

    public synchronized void stop()
    {
        running = false;
        if (releaseKey(true))
            listener.onPtt(false, "");
        RemoteConnection c = conn;
        if (c != null)
            c.close();
        if (loop != null)
            loop.interrupt();
        listener.onLink("offline", "");
    }

    private void runLoop()
    {
        int attempt = 0;

        while (running)
        {
            listener.onLink(attempt == 0 ? "connecting" : "reconnecting", "");

            final CountDownLatch ended = new CountDownLatch(1);
            RemoteConnection c = new RemoteConnection(host, port, token, pinned, new RemoteConnection.Listener()
            {
                public void onEvent(Map<String, Object> e)
                {
                    handleEvent(e);
                }

                public void onAudio(short[] pcm, int count)
                {
                    if (audioOn)
                        speaker.play(pcm, count);
                }

                public void onClosed(String reason)
                {
                    ended.countDown();
                }
            });

            try
            {
                c.connect(8000);
            }
            catch (RemoteConnection.RemoteException e)
            {
                if (e.kind == RemoteConnection.KIND_PIN || e.kind == RemoteConnection.KIND_AUTH || e.kind == RemoteConnection.KIND_PROOF)
                {
                    running = false;
                    listener.onLink("failed", e.getMessage());
                    return;
                }
                attempt = wait(attempt);
                continue;
            }
            catch (IOException e)
            {
                attempt = wait(attempt);
                continue;
            }

            conn = c;
            attempt = 1;
            listener.onLink("online", "");
            try
            {
                if (audioOn)
                    startAudioOn(c);
                if (monitorOn)
                    c.call("monitor_start", Json.map("intervalMs", 500), 5000);
            }
            catch (IOException e)
            {
                c.close();
            }

            try
            {
                ended.await();
            }
            catch (InterruptedException e)
            {
                c.close();
                return;
            }
            conn = null;
            if (releaseKey(false))
                listener.onPtt(false, "The connection to the radio was lost");
            speaker.clear();
            if (running)
            {
                listener.onLink("reconnecting", "");
                attempt = wait(1);
            }
        }
    }

    private int wait(int attempt)
    {
        long ms = Math.min((long)reconnectMaxMs, 1000L << Math.min(attempt, 5));
        try
        {
            if (running)
                Thread.sleep(ms);
        }
        catch (InterruptedException e)
        {
            running = false;
        }
        return attempt + 1;
    }

    private void startAudioOn(RemoteConnection c) throws IOException
    {
        c.call("audio_start", Json.map("codec", ulaw ? "ulaw" : "pcm16", "rx", Boolean.TRUE, "tx", Boolean.TRUE), 5000);
    }

    private void handleEvent(Map<String, Object> e)
    {
        String type = Json.str(e, "type", "");

        if ("state".equals(type))
        {
            lastState = e;
            listener.onState(e);
        }
        else if ("call".equals(type))
            listener.onCall(e);
        else if ("log".equals(type))
            listener.onLog(Json.str(e, "text", ""), (long)Json.num(e, "time", System.currentTimeMillis()));
        else if ("monitor".equals(type))
            listener.onMonitor(e);
        else if ("ptt".equals(type))
        {
            /* the radio took the key away (no keepalive, time limit, the desktop took it ...) */
            if (holding && !"remote".equals(Json.str(e, "owner", "")))
            {
                releaseKey(false);
                String reason = Json.str(e, "reason", "");
                listener.onPtt(false, reason.length() > 0 ? "The key was released: " + reason : "The key was released");
            }
        }
    }

    /* ---------------------------------------------------------------------------------------------- commands */

    /** a command, run on a worker thread; the result is reported there too */
    public void command(final String cmd, final Map<String, Object> args, final long timeoutMs, final Result result)
    {
        exec.execute(new Runnable()
        {
            public void run()
            {
                RemoteConnection c = conn;
                if (c == null || !c.isOpen())
                {
                    if (result != null)
                        result.done(false, null, "not connected to the radio");
                    return;
                }
                try
                {
                    Map<String, Object> r = c.call(cmd, args, timeoutMs);
                    boolean ok = Json.bool(r, "ok", false);
                    if (result != null)
                        result.done(ok, Json.obj(r.get("result")), ok ? "" : Json.str(r, "error", "failed"));
                }
                catch (IOException e)
                {
                    if (result != null)
                        result.done(false, null, e.getMessage());
                }
            }
        });
    }

    public void setMonitor(boolean on)
    {
        monitorOn = on;
        command(on ? "monitor_start" : "monitor_stop", on ? Json.map("intervalMs", 500) : null, 5000, null);
    }

    /** mutes (false) or unmutes the speech from the radio; the codec applies from the next unmute */
    public void setAudio(boolean on, boolean useUlaw)
    {
        audioOn = on;
        ulaw = useUlaw;
        if (!on)
            speaker.clear();
        command(on ? "audio_start" : "audio_stop", on ? Json.map("codec", useUlaw ? "ulaw" : "pcm16", "rx", Boolean.TRUE, "tx", Boolean.TRUE) : null,
                5000, null);
    }

    /* ---------------------------------------------------------------------------------------------- push to talk */

    /** the key goes down (when the radio allows it): the result comes through Listener.onPtt */
    public void pttDown()
    {
        wantHold = true;
        exec.execute(new Runnable()
        {
            public void run()
            {
                if (holding || !wantHold)
                    return;
                RemoteConnection c = conn;
                if (c == null || !c.isOpen())
                {
                    wantHold = false;
                    listener.onPtt(false, "not connected to the radio");
                    return;
                }
                try
                {
                    Map<String, Object> r = c.call("ptt", Json.map("down", Boolean.TRUE), 5000);
                    if (!Json.bool(r, "ok", false))
                    {
                        wantHold = false;
                        listener.onPtt(false, Json.str(r, "error", "refused"));
                        return;
                    }
                    if (!wantHold)                      /* the finger left before the answer came */
                    {
                        c.post("ptt", Json.map("down", Boolean.FALSE));
                        return;
                    }
                    synchronized (pttLock)
                    {
                        holding = true;
                        keepalive = new Thread(new Runnable()
                        {
                            public void run()
                            {
                                keepaliveLoop();
                            }
                        }, "openmt-keepalive");
                        keepalive.setDaemon(true);
                        keepalive.start();
                    }
                    microphone.start();
                    listener.onPtt(true, "");
                }
                catch (IOException e)
                {
                    wantHold = false;
                    listener.onPtt(false, e.getMessage());
                }
            }
        });
    }

    public void pttUp()
    {
        wantHold = false;
        exec.execute(new Runnable()
        {
            public void run()
            {
                if (holding)
                {
                    releaseKey(true);
                    listener.onPtt(false, "");
                }
            }
        });
    }

    /** stops the key on this side; tell = also tell the radio */
    private boolean releaseKey(boolean tell)
    {
        boolean was;

        synchronized (pttLock)
        {
            was = holding;
            holding = false;
            wantHold = false;
            if (keepalive != null)
            {
                keepalive.interrupt();
                keepalive = null;
            }
        }
        if (was)
        {
            microphone.stop();
            RemoteConnection c = conn;
            if (tell && c != null && c.isOpen())
            {
                try
                {
                    c.post("ptt", Json.map("down", Boolean.FALSE));
                }
                catch (IOException ignored)
                {
                }
            }
        }
        return was;
    }

    private void keepaliveLoop()
    {
        while (holding)
        {
            try
            {
                Thread.sleep(300);
            }
            catch (InterruptedException e)
            {
                return;
            }
            RemoteConnection c = conn;
            if (!holding)
                return;
            try
            {
                if (c == null)
                    throw new IOException("no connection");
                c.post("ptt_keepalive", null);
            }
            catch (IOException e)
            {
                releaseKey(false);
                listener.onPtt(false, "The connection to the radio was lost");
                return;
            }
        }
    }

    /** microphone speech, 8 kHz, called by the Microphone every 20 ms while the key is down */
    public void feedMic(short[] pcm, int off, int count)
    {
        RemoteConnection c = conn;
        if (!holding || c == null)
            return;
        try
        {
            c.sendAudio(pcm, off, count, ulaw);
        }
        catch (IOException ignored)
        {
        }
    }
}
