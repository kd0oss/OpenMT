package org.openmt.radio.core;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.security.cert.Certificate;
import java.security.cert.X509Certificate;
import java.util.Base64;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

/** One connection to an OpenMT Radio (protocol: REMOTE_PROTOCOL.md): TLS with the certificate pinned by its SHA-256 fingerprint,
 *  WebSocket, the challenge response login (the radio's proof is checked), commands with replies, events and audio frames.
 *  No Android classes in it: it runs on a plain JVM, where tests/android_core_test.py runs it against the real server. */
public final class RemoteConnection
{
    public static final int KIND_PIN = 1;           /* the certificate is not the pinned one */
    public static final int KIND_AUTH = 2;          /* the radio refused the token */
    public static final int KIND_PROOF = 3;         /* the radio did not prove it knows the token */
    public static final int KIND_PROTOCOL = 4;
    public static final int KIND_IO = 5;

    public static final class RemoteException extends IOException
    {
        public final int kind;

        public RemoteException(int kind, String message)
        {
            super(message);
            this.kind = kind;
        }
    }

    public interface Listener
    {
        /** state, call, log, ptt, monitor, error ... (called on the reader thread) */
        void onEvent(Map<String, Object> event);

        /** speech from the radio: 8 kHz, 160 samples (or what the radio sent) */
        void onAudio(short[] pcm, int count);

        /** the connection ended (the reason is only for the log) */
        void onClosed(String reason);
    }

    private static final String GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final SecureRandom RANDOM = new SecureRandom();

    private final String host;
    private final int port;
    private final String token;
    private final String pinned;
    private final Listener listener;

    private SSLSocket sock;
    private BufferedInputStream in;
    private OutputStream out;
    private final Object writeLock = new Object();
    private volatile boolean closed;
    private volatile String closeReason = "closed";
    private final AtomicInteger nextId = new AtomicInteger(1);
    private final Map<Integer, Pending> pending = new ConcurrentHashMap<Integer, Pending>();
    private int txSeq;
    private Thread reader;
    private Thread pinger;

    public String fingerprint = "";
    public String serverName = "";
    public int clientId;

    private static final class Pending
    {
        final CountDownLatch done = new CountDownLatch(1);
        volatile Map<String, Object> reply;
    }

    private static final class Message
    {
        boolean text;
        byte[] data;
    }

    /** pinnedFingerprint: "AA:BB:..." or null (the first time: use fetchFingerprint() to show it to the person, then pin it) */
    public RemoteConnection(String host, int port, String token, String pinnedFingerprint, Listener listener)
    {
        this.host = host;
        this.port = port;
        this.token = normalizeToken(token);
        this.pinned = pinnedFingerprint == null || pinnedFingerprint.length() == 0 ? null : pinnedFingerprint.toUpperCase();
        this.listener = listener;
    }

    public static String normalizeToken(String text)
    {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < text.length(); i++)
        {
            char c = text.charAt(i);
            if (Character.isLetterOrDigit(c))
                sb.append(Character.toUpperCase(c));
        }
        return sb.toString();
    }

    /* ---------------------------------------------------------------------------------------------- TLS */

    private static SSLSocket openTls(String host, int port, int timeoutMs) throws IOException
    {
        try
        {
            /* the certificate is self signed: it is not checked against authorities but pinned by its fingerprint afterwards */
            TrustManager[] any = new TrustManager[] { new X509TrustManager()
            {
                public void checkClientTrusted(X509Certificate[] chain, String authType)
                {
                }

                public void checkServerTrusted(X509Certificate[] chain, String authType)
                {
                }

                public X509Certificate[] getAcceptedIssuers()
                {
                    return new X509Certificate[0];
                }
            } };
            SSLContext ctx = SSLContext.getInstance("TLS");
            ctx.init(null, any, RANDOM);

            Socket raw = new Socket();
            raw.connect(new InetSocketAddress(host, port), timeoutMs);
            raw.setTcpNoDelay(true);
            raw.setSoTimeout(timeoutMs);

            SSLSocket s = (SSLSocket)ctx.getSocketFactory().createSocket(raw, host, port, true);
            s.setEnabledProtocols(supported(s));
            s.startHandshake();
            return s;
        }
        catch (IOException e)
        {
            throw e;
        }
        catch (Exception e)
        {
            throw new IOException("TLS: " + e);
        }
    }

    private static String[] supported(SSLSocket s)
    {
        java.util.List<String> ok = new java.util.ArrayList<String>();
        for (String p : s.getSupportedProtocols())
            if (p.equals("TLSv1.3") || p.equals("TLSv1.2"))
                ok.add(p);
        return ok.toArray(new String[0]);
    }

    private static String fingerprintOf(SSLSocket s) throws IOException
    {
        try
        {
            Certificate[] chain = s.getSession().getPeerCertificates();
            byte[] der = chain[0].getEncoded();
            byte[] h = MessageDigest.getInstance("SHA-256").digest(der);
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < h.length; i++)
            {
                if (i > 0)
                    sb.append(':');
                sb.append(String.format("%02X", h[i] & 0xFF));
            }
            return sb.toString();
        }
        catch (Exception e)
        {
            throw new IOException("certificate: " + e);
        }
    }

    /** The radio's certificate fingerprint, to show to the person the first time (nothing is sent but the TLS handshake) */
    public static String fetchFingerprint(String host, int port, int timeoutMs) throws IOException
    {
        SSLSocket s = openTls(host, port, timeoutMs);
        try
        {
            return fingerprintOf(s);
        }
        finally
        {
            try
            {
                s.close();
            }
            catch (IOException ignored)
            {
            }
        }
    }

    /* ---------------------------------------------------------------------------------------------- login */

    private static String hex(byte[] b)
    {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < b.length; i++)
            sb.append(String.format("%02x", b[i] & 0xFF));
        return sb.toString();
    }

    public static String authMac(String token, String prefix, String nonce, String cnonce)
    {
        try
        {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(token.getBytes(UTF8), "HmacSHA256"));
            return hex(mac.doFinal((prefix + nonce + cnonce).getBytes(UTF8)));
        }
        catch (Exception e)
        {
            throw new IllegalStateException(e);
        }
    }

    /** Connects and logs in (blocking, a few seconds at most).  Throws a RemoteException that says what was wrong. */
    public void connect(int timeoutMs) throws IOException
    {
        sock = openTls(host, port, timeoutMs);
        try
        {
            fingerprint = fingerprintOf(sock);
            if (pinned != null && !pinned.equals(fingerprint))
                throw new RemoteException(KIND_PIN, "the radio's certificate is not the one you trusted (" + fingerprint + ")");

            in = new BufferedInputStream(sock.getInputStream(), 65536);
            out = sock.getOutputStream();

            byte[] k = new byte[16];
            RANDOM.nextBytes(k);
            String key = Base64.getEncoder().encodeToString(k);
            String req = "GET /openmt HTTP/1.1\r\nHost: " + host + ":" + port + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                       + "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
            out.write(req.getBytes(UTF8));
            out.flush();

            ByteArrayOutputStream head = new ByteArrayOutputStream();
            while (true)
            {
                int b = in.read();
                if (b < 0)
                    throw new EOFException("the radio closed the connection");
                head.write(b);
                byte[] h = head.toByteArray();
                int n = h.length;
                if (n >= 4 && h[n - 4] == '\r' && h[n - 3] == '\n' && h[n - 2] == '\r' && h[n - 1] == '\n')
                    break;
                if (n > 8192)
                    throw new RemoteException(KIND_PROTOCOL, "the answer to the upgrade is too long");
            }
            String response = new String(head.toByteArray(), UTF8);
            String accept = Base64.getEncoder().encodeToString(
                MessageDigest.getInstance("SHA-1").digest((key + GUID).getBytes(UTF8)));
            if (!response.startsWith("HTTP/1.1 101") || !response.contains("Sec-WebSocket-Accept: " + accept))
                throw new RemoteException(KIND_PROTOCOL, "the radio did not accept the WebSocket: " + response.split("\r\n")[0]);

            Map<String, Object> hello = Json.obj(Json.parse(new String(nextMessage().data, UTF8)));
            if (hello == null || !"hello".equals(Json.str(hello, "type", "")))
                throw new RemoteException(KIND_PROTOCOL, "no hello from the radio");
            serverName = Json.str(hello, "server", "");
            String nonce = Json.str(hello, "nonce", "");
            byte[] cn = new byte[16];
            RANDOM.nextBytes(cn);
            String cnonce = hex(cn);

            sendText(Json.dump(Json.map("type", "auth", "cnonce", cnonce, "response", authMac(token, "C", nonce, cnonce))));

            Map<String, Object> ok = Json.obj(Json.parse(new String(nextMessage().data, UTF8)));
            String type = Json.str(ok, "type", "");
            if ("auth_failed".equals(type))
                throw new RemoteException(KIND_AUTH, ok.containsKey("error") ? "refused: " + Json.str(ok, "error", "") : "wrong token");
            if (!"auth_ok".equals(type))
                throw new RemoteException(KIND_PROTOCOL, "unexpected answer to the login: " + type);
            byte[] want = authMac(token, "S", nonce, cnonce).getBytes(UTF8);
            byte[] got = Json.str(ok, "proof", "").getBytes(UTF8);
            if (!MessageDigest.isEqual(want, got))
                throw new RemoteException(KIND_PROOF, "the radio did not prove that it knows the token");
            clientId = (int)Json.num(ok, "clientId", 0);

            sock.setSoTimeout(0);
        }
        catch (IOException e)
        {
            abort("login failed");
            throw e;
        }
        catch (Exception e)
        {
            abort("login failed");
            throw new RemoteException(KIND_PROTOCOL, String.valueOf(e));
        }

        reader = new Thread(new Runnable()
        {
            public void run()
            {
                readLoop();
            }
        }, "openmt-reader");
        reader.setDaemon(true);
        reader.start();

        pinger = new Thread(new Runnable()
        {
            public void run()
            {
                while (!closed)
                {
                    try
                    {
                        Thread.sleep(10000);
                    }
                    catch (InterruptedException e)
                    {
                        return;
                    }
                    if (!closed)
                    {
                        try
                        {
                            sendText(Json.dump(Json.map("id", nextId.getAndIncrement(), "cmd", "ping")));
                        }
                        catch (IOException e)
                        {
                            return;
                        }
                    }
                }
            }
        }, "openmt-ping");
        pinger.setDaemon(true);
        pinger.start();
    }

    /* ---------------------------------------------------------------------------------------------- frames */

    private void sendFrame(int opcode, byte[] payload, int off, int len) throws IOException
    {
        byte[] head = new byte[14];
        int n = 0;
        head[n++] = (byte)(0x80 | opcode);
        if (len < 126)
            head[n++] = (byte)(0x80 | len);
        else if (len < 65536)
        {
            head[n++] = (byte)(0x80 | 126);
            head[n++] = (byte)(len >> 8);
            head[n++] = (byte)len;
        }
        else
        {
            head[n++] = (byte)(0x80 | 127);
            for (int i = 7; i >= 0; i--)
                head[n++] = (byte)(((long)len >> (8 * i)) & 0xFF);
        }
        byte[] mask = new byte[4];
        RANDOM.nextBytes(mask);
        System.arraycopy(mask, 0, head, n, 4);
        n += 4;
        byte[] body = new byte[len];
        for (int i = 0; i < len; i++)
            body[i] = (byte)(payload[off + i] ^ mask[i & 3]);
        synchronized (writeLock)
        {
            if (out == null)
                throw new IOException("not connected");
            out.write(head, 0, n);
            out.write(body);
            out.flush();
        }
    }

    private void sendText(String json) throws IOException
    {
        byte[] b = json.getBytes(UTF8);
        sendFrame(1, b, 0, b.length);
    }

    private void readFully(byte[] b, int len) throws IOException
    {
        int got = 0;
        while (got < len)
        {
            int r = in.read(b, got, len - got);
            if (r < 0)
                throw new EOFException("the radio closed the connection");
            got += r;
        }
    }

    /** the next text or binary message (fragments put together; pings answered; a close ends it with an exception) */
    private Message nextMessage() throws IOException
    {
        ByteArrayOutputStream acc = new ByteArrayOutputStream();
        boolean text = false;
        boolean started = false;

        for (;;)
        {
            byte[] h = new byte[2];
            readFully(h, 2);
            boolean fin = (h[0] & 0x80) != 0;
            int op = h[0] & 15;
            long len = h[1] & 0x7F;
            if (len == 126)
            {
                byte[] e = new byte[2];
                readFully(e, 2);
                len = ((e[0] & 0xFF) << 8) | (e[1] & 0xFF);
            }
            else if (len == 127)
            {
                byte[] e = new byte[8];
                readFully(e, 8);
                len = 0;
                for (int i = 0; i < 8; i++)
                    len = (len << 8) | (e[i] & 0xFF);
            }
            if (len > 1024 * 1024)
                throw new RemoteException(KIND_PROTOCOL, "a message is too long");
            byte[] payload = new byte[(int)len];
            readFully(payload, (int)len);

            if (op == 8)
            {
                int code = payload.length >= 2 ? ((payload[0] & 0xFF) << 8) | (payload[1] & 0xFF) : 1005;
                closeReason = "the radio closed the connection (" + code + ")";
                throw new EOFException(closeReason);
            }
            if (op == 9)
            {
                sendFrame(10, payload, 0, payload.length);
                continue;
            }
            if (op == 10)
                continue;
            if (op == 1 || op == 2)
            {
                acc.reset();
                text = op == 1;
                started = true;
            }
            else if (!started)
                throw new RemoteException(KIND_PROTOCOL, "a continuation frame without a start");
            acc.write(payload, 0, payload.length);
            if (fin)
            {
                Message m = new Message();
                m.text = text;
                m.data = acc.toByteArray();
                return m;
            }
        }
    }

    private void readLoop()
    {
        try
        {
            for (;;)
            {
                Message m = nextMessage();
                if (m.text)
                {
                    Map<String, Object> o;
                    try
                    {
                        o = Json.obj(Json.parse(new String(m.data, UTF8)));
                    }
                    catch (IllegalArgumentException e)
                    {
                        continue;
                    }
                    if (o == null)
                        continue;
                    if ("reply".equals(Json.str(o, "type", "")))
                    {
                        Pending p = pending.remove((int)Json.num(o, "id", -1));
                        if (p != null)
                        {
                            p.reply = o;
                            p.done.countDown();
                            continue;
                        }
                    }
                    listener.onEvent(o);
                }
                else
                    deliverAudio(m.data);
            }
        }
        catch (IOException e)
        {
            if (!closed && closeReason.equals("closed"))
                closeReason = String.valueOf(e.getMessage());
        }
        finish();
    }

    private void deliverAudio(byte[] d)
    {
        if (d.length < 5 || d[0] != 1)
            return;
        int codec = d[1];
        short[] pcm;
        int n;
        if (codec == 1)
        {
            n = d.length - 4;
            pcm = new short[n];
            for (int i = 0; i < n; i++)
                pcm[i] = G711.decode(d[4 + i]);
        }
        else
        {
            n = (d.length - 4) / 2;
            pcm = new short[n];
            for (int i = 0; i < n; i++)
                pcm[i] = (short)((d[4 + 2 * i] & 0xFF) | (d[5 + 2 * i] << 8));
        }
        listener.onAudio(pcm, n);
    }

    private void finish()
    {
        closed = true;
        for (Pending p : pending.values())
            p.done.countDown();
        pending.clear();
        try
        {
            if (sock != null)
                sock.close();
        }
        catch (IOException ignored)
        {
        }
        listener.onClosed(closeReason);
    }

    private void abort(String why)
    {
        closed = true;
        closeReason = why;
        try
        {
            if (sock != null)
                sock.close();
        }
        catch (IOException ignored)
        {
        }
    }

    /* ---------------------------------------------------------------------------------------------- use */

    public boolean isOpen()
    {
        return !closed && sock != null && !sock.isClosed();
    }

    /** a command and its reply ({"ok":true,"result":{...}} or {"ok":false,"error":"..."}); blocks up to timeoutMs */
    public Map<String, Object> call(String cmd, Map<String, Object> args, long timeoutMs) throws IOException
    {
        int id = nextId.getAndIncrement();
        Map<String, Object> m = Json.map("id", id, "cmd", cmd);
        if (args != null)
            m.putAll(args);
        Pending p = new Pending();
        pending.put(id, p);
        try
        {
            sendText(Json.dump(m));
            if (!p.done.await(timeoutMs, TimeUnit.MILLISECONDS))
            {
                pending.remove(id);
                throw new IOException("no answer to " + cmd);
            }
        }
        catch (InterruptedException e)
        {
            pending.remove(id);
            throw new IOException("interrupted");
        }
        if (p.reply == null)
            throw new IOException("the connection was lost");
        return p.reply;
    }

    /** a command without waiting for the reply (keepalives, release) */
    public void post(String cmd, Map<String, Object> args) throws IOException
    {
        Map<String, Object> m = Json.map("cmd", cmd);
        if (args != null)
            m.putAll(args);
        sendText(Json.dump(m));
    }

    /** microphone speech to the radio (only used while this client holds the key); 8 kHz */
    public void sendAudio(short[] pcm, int off, int count, boolean ulaw) throws IOException
    {
        byte[] b = new byte[4 + (ulaw ? count : 2 * count)];
        b[0] = 2;
        b[1] = (byte)(ulaw ? 1 : 0);
        b[2] = (byte)(txSeq >> 8);
        b[3] = (byte)txSeq;
        txSeq = (txSeq + 1) & 0xFFFF;
        for (int i = 0; i < count; i++)
        {
            short s = pcm[off + i];
            if (ulaw)
                b[4 + i] = G711.encode(s);
            else
            {
                b[4 + 2 * i] = (byte)s;
                b[5 + 2 * i] = (byte)(s >> 8);
            }
        }
        sendFrame(2, b, 0, b.length);
    }

    /** closes the connection (with a goodbye) */
    public void close()
    {
        if (!closed)
        {
            closeReason = "closed";
            try
            {
                sendFrame(8, new byte[] { 0x03, (byte)0xE8 }, 0, 2);
            }
            catch (IOException ignored)
            {
            }
        }
        closed = true;
        try
        {
            if (sock != null)
                sock.close();
        }
        catch (IOException ignored)
        {
        }
    }
}
