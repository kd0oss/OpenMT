import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

import org.openmt.radio.core.G711;
import org.openmt.radio.core.Json;
import org.openmt.radio.core.RadioClient;
import org.openmt.radio.core.RemoteConnection;

/** Runs the protocol code of the Android app on a plain JVM against the real server: java CoreTest HOST PORT TOKEN FINGERPRINT.
 *  Prints "CHECK ok|FAIL text" lines and "CUE x" lines (the driver, android_core_test.py, answers a cue by making the server do
 *  something) and exits with the number of failures. */
public class CoreTest
{
    static int failures, checks;

    static synchronized void check(boolean ok, String what)
    {
        checks++;
        if (!ok)
            failures++;
        System.out.println("CHECK " + (ok ? "ok " : "FAIL ") + what);
        System.out.flush();
    }

    static void cue(String what)
    {
        System.out.println("CUE " + what);
        System.out.flush();
    }

    static boolean until(long ms, java.util.concurrent.Callable<Boolean> c) throws Exception
    {
        long end = System.currentTimeMillis() + ms;
        while (System.currentTimeMillis() < end)
        {
            if (c.call())
                return true;
            Thread.sleep(20);
        }
        return c.call();
    }

    static final List<String> links = new ArrayList<String>();
    static final List<String> linkMessages = new ArrayList<String>();
    static volatile Map<String, Object> lastCall;
    static volatile boolean holding;
    static volatile String pttMessage = "";
    static final AtomicInteger spoken = new AtomicInteger();
    static volatile int firstSample = -1;
    static final AtomicInteger micStarts = new AtomicInteger(), micStops = new AtomicInteger();
    static volatile boolean micRunning;
    static final List<String> logs = new ArrayList<String>();

    static RadioClient make(final String tag)
    {
        return new RadioClient(new RadioClient.Listener()
        {
            public void onLink(String state, String message)
            {
                synchronized (links)
                {
                    links.add(state);
                    linkMessages.add(message);
                }
            }

            public void onState(Map<String, Object> s)
            {
            }

            public void onCall(Map<String, Object> e)
            {
                lastCall = e;
            }

            public void onLog(String text, long t)
            {
                synchronized (logs)
                {
                    logs.add(text);
                }
            }

            public void onPtt(boolean h, String message)
            {
                holding = h;
                pttMessage = message;
            }

            public void onMonitor(Map<String, Object> m)
            {
            }
        }, new RadioClient.Speaker()
        {
            public void play(short[] pcm, int n)
            {
                if (spoken.getAndIncrement() == 0)
                    firstSample = pcm[0];
            }

            public void clear()
            {
            }
        }, new RadioClient.Microphone()
        {
            public void start()
            {
                micStarts.incrementAndGet();
                micRunning = true;
            }

            public void stop()
            {
                micStops.incrementAndGet();
                micRunning = false;
            }
        });
    }

    static boolean has(String state)
    {
        synchronized (links)
        {
            return links.contains(state);
        }
    }

    static boolean last(String state)
    {
        synchronized (links)
        {
            return !links.isEmpty() && links.get(links.size() - 1).equals(state);
        }
    }

    public static void main(String[] a) throws Exception
    {
        String host = a[0];
        int port = Integer.parseInt(a[1]);
        String token = a[2];
        String fp = a[3];

        /* the pieces */
        check(RemoteConnection.authMac("ABCDEFGHJKLMNPQRSTUVWXYZ", "C", "00112233445566778899aabbccddeeff", "ffeeddccbbaa99887766554433221100")
              .equals("24babe65aec78bdfc7d810dd8f438e942c3c29cca803cf93e2f0c0230d85fb73"), "the login test vector");
        check(RemoteConnection.normalizeToken("abcd-efgh jklm").equals("ABCDEFGHJKLM"), "token normalising");
        boolean ulawOk = true;
        for (int v = -32000; v <= 32000; v += 997)
            ulawOk &= Math.abs(G711.decode(G711.encode(v)) - v) <= Math.max(16, Math.abs(v) / 15);
        check(ulawOk, "mu-law round trip");
        Object j = Json.parse("{\"a\":[1,2.5,\"t\\u00e9st\\n\"],\"b\":{\"c\":true,\"d\":null}}");
        check(Json.dump(j).equals("{\"a\":[1,2.5,\"tést\\n\"],\"b\":{\"c\":true,\"d\":null}}"), "JSON round trip");

        /* the certificate */
        check(RemoteConnection.fetchFingerprint(host, port, 3000).equalsIgnoreCase(fp), "the fingerprint the app shows is the radio's");

        RemoteConnection.Listener none = new RemoteConnection.Listener()
        {
            public void onEvent(Map<String, Object> e) {}
            public void onAudio(short[] p, int n) {}
            public void onClosed(String r) {}
        };
        try
        {
            new RemoteConnection(host, port, token, "00:11:22", none).connect(3000);
            check(false, "a wrong pin is refused");
        }
        catch (RemoteConnection.RemoteException e)
        {
            check(e.kind == RemoteConnection.KIND_PIN, "a wrong certificate pin is refused");
        }
        try
        {
            new RemoteConnection(host, port, "AAAA-BBBB-CCCC-DDDD-EEEE-FFFF", fp, none).connect(3000);
            check(false, "a wrong token is refused");
        }
        catch (RemoteConnection.RemoteException e)
        {
            check(e.kind == RemoteConnection.KIND_AUTH, "a wrong token is refused");
        }

        /* a client that has a bad token stops trying; one that cannot reach the radio keeps trying */
        RadioClient bad = make("bad");
        bad.setTarget(host, port, "AAAA-BBBB-CCCC-DDDD-EEEE-FFFF", fp);
        bad.start();
        check(until(5000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return has("failed"); } }),
              "a wrong token ends the attempts with 'failed'");
        bad.stop();
        synchronized (links) { links.clear(); }

        RadioClient lost = make("lost");
        lost.reconnectMaxMs = 200;
        lost.setTarget("127.0.0.1", 1, token, fp);          /* nothing listens on port 1 */
        lost.start();
        check(until(5000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return has("reconnecting"); } }),
              "an unreachable radio is retried ('reconnecting')");
        check(!has("failed"), "an unreachable radio is not a fatal failure");
        lost.stop();
        synchronized (links) { links.clear(); }

        /* the real thing */
        final RadioClient rc = make("main");
        rc.setTarget(host, port, token, fp);
        rc.start();
        check(until(5000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return has("online"); } }), "online");
        check(until(3000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return rc.state() != null; } }), "the state arrives after the login");

        final Object[] res = new Object[3];
        final Object lock = new Object();
        RadioClient.Result r = new RadioClient.Result()
        {
            public void done(boolean ok, Map<String, Object> result, String error)
            {
                synchronized (lock)
                {
                    res[0] = ok;
                    res[1] = result;
                    res[2] = error;
                    lock.notifyAll();
                }
            }
        };
        class Run
        {
            boolean ok(String cmd, Map<String, Object> args, long ms) throws Exception
            {
                synchronized (lock)
                {
                    res[0] = null;
                }
                rc.command(cmd, args, ms, r);
                synchronized (lock)
                {
                    long end = System.currentTimeMillis() + ms + 1000;
                    while (res[0] == null && System.currentTimeMillis() < end)
                        lock.wait(100);
                }
                return Boolean.TRUE.equals(res[0]);
            }
        }
        Run run = new Run();

        check(run.ok("connect", null, 10000), "connect command");
        check(until(3000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return Json.bool(rc.state(), "connected", false); } }),
              "the state says connected");

        check(run.ok("get_settings", null, 5000) && Json.obj(Json.obj(res[1]).get("settings")) != null, "get_settings");
        check(run.ok("get_options", null, 5000), "get_options");
        check(run.ok("set_settings", Json.map("settings", Json.map("m17Meta", "from android")), 5000), "set a live setting while connected");
        check(!run.ok("set_settings", Json.map("settings", Json.map("host", "elsewhere")), 5000) && String.valueOf(res[2]).length() > 0,
              "a setting that needs a disconnected radio is refused with a message");
        check(run.ok("dtmf", Json.map("digit", "7"), 5000), "dtmf");
        check(run.ok("set_meta", Json.map("text", "from android"), 5000), "set_meta");
        check(run.ok("send_message", Json.map("to", "ALL", "text", "hello"), 5000), "send_message");

        /* speech and events from the radio */
        cue("audio");
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return spoken.get() >= 1; } }), "speech from the radio reaches the speaker");
        check(Math.abs(firstSample - 777) < 40, "the speech is decoded right (" + firstSample + ")");
        cue("call");
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return lastCall != null; } })
              && (int)Json.num(lastCall, "src", 0) == 1234 && "tést".equals(Json.str(lastCall, "text", "")), "a call event with its text");
        cue("logline");
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { synchronized (logs) { return logs.contains("hello from the radio"); } } }),
              "log lines arrive");

        /* push to talk */
        rc.pttDown();
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return holding && micRunning; } }), "the key goes down and the microphone starts");
        short[] frame = new short[160];
        for (int i = 0; i < 160; i++)
            frame[i] = (short)(3000 * Math.sin(i * 0.3));
        for (int i = 0; i < 40; i++)                                    /* 800 ms of speech: the keepalive must hold the key too */
        {
            rc.feedMic(frame, 0, 160);
            Thread.sleep(20);
        }
        Thread.sleep(1700);                                             /* silence of the microphone: the keepalives alone keep the key */
        check(holding, "the key is still held after 1.7 s without speech (keepalive)");
        rc.pttUp();
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return !holding && !micRunning; } }), "the key is released and the microphone stops");
        cue("micstat");

        /* the radio takes the key away when we go silent: stop the keepalive by closing from outside is not possible here, so
           test the other side: stop() while holding must release */
        rc.pttDown();
        check(until(4000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return holding; } }), "the key goes down again");
        rc.stop();
        check(until(3000, new java.util.concurrent.Callable<Boolean>() { public Boolean call() { return !holding && !micRunning; } }), "stopping the client releases the key");
        cue("done");

        System.out.println("TOTAL " + checks + " " + failures);
        System.out.flush();
        System.exit(failures);
    }
}
