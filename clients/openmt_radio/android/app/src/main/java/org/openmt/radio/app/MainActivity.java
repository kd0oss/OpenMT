package org.openmt.radio.app;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Pattern;

import org.openmt.radio.core.Json;
import org.openmt.radio.core.RadioClient;
import org.openmt.radio.core.RemoteConnection;

/** The screen of the app: pairing with a radio, then the same controls as the window of OpenMT Radio (Radio, Settings, Monitor, Log).
 *  The connection itself lives in RadioService; this class only shows what the service reports and sends the person's commands. */
public final class MainActivity extends Activity implements RadioService.Ui
{
    private static final String[][] SECTIONS = {
        { "Connection", "^(host|modem|mode|layout)$" }, { "NXDN", "^(ran|src|dst|group)$" }, { "DMR", "^dmr.*" }, { "P25", "^p25.*" },
        { "YSF", "^ysf.*" }, { "D-STAR", "^dstar.*" }, { "M17", "^m17.*" }, { "FM", "^fm.*" },
        { "Vocoder", "^(ambe|noVocoder|capture|invertRx).*" },
        { "Audio and levels", "^(txLevel|rxLevel|spectrum|audioIn|audioOut|rate|micGain|spkGain).*" }
    };
    private static final String LIVE = ",micGain,spkGain,m17Meta,m17MsgTo,";

    private Prefs prefs;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private RadioService service;
    private boolean bound;
    private boolean refreshPending;

    private LinearLayout root;
    private TextView linkLine;
    private LinearLayout tabs;
    private FrameLayout content;
    private View pairPanel;
    private View[] panels = new View[4];
    private Button[] tabButtons = new Button[4];
    private int tab;

    /* Radio */
    private TextView modeLine, statusLine, heardLine, callLine, cmdErr, pttNote, audioInfo;
    private Button linkBtn, connectBtn, disconnectBtn, releaseBtn, dongleBtn, pttBtn, muteBtn;
    private View msgCard;
    private EditText msgTo, msgText, metaText;
    private CheckBox latch, volKey;

    /* Settings */
    private LinearLayout setForm;
    private TextView setErr;
    private final List<Field> fields = new ArrayList<Field>();
    private Map<String, Object> currentSettings = new LinkedHashMap<String, Object>();

    /* Monitor and log */
    private TextView monText, logView;
    private LevelsView levels;
    private ScrollView logScroll;

    /* Pairing */
    private EditText pairHost, pairPort, pairToken;
    private TextView pairErr;

    private static final class Field
    {
        String key;
        char type;                      /* b boolean, n number, s text, c choice */
        String original;
        View view;
        List<String> values;            /* for a choice */
    }

    /* ------------------------------------------------------------------------------------------------ lifecycle */

    @Override
    protected void onCreate(Bundle b)
    {
        super.onCreate(b);
        prefs = new Prefs(this);
        buildUi();

        List<String> need = new ArrayList<String>();
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
            need.add(Manifest.permission.RECORD_AUDIO);
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            need.add(Manifest.permission.POST_NOTIFICATIONS);
        if (!need.isEmpty())
            requestPermissions(need.toArray(new String[0]), 1);

        showMode();
        if (prefs.paired() && !prefs.linkOff())
            startRadio();
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] results)
    {
        if (prefs.paired() && !prefs.linkOff())
            startRadio();                   /* again: the service then also takes the microphone type */
    }

    @Override
    protected void onStart()
    {
        super.onStart();
        if (prefs.paired())
            bindRadio();
    }

    @Override
    protected void onStop()
    {
        if (service != null)
        {
            service.client().pttUp();       /* the key never stays down in the background */
            service.setUi(null);
            service.client().setMonitor(false);
        }
        if (bound)
        {
            unbindService(connection);
            bound = false;
        }
        service = null;
        super.onStop();
    }

    private final ServiceConnection connection = new ServiceConnection()
    {
        public void onServiceConnected(ComponentName n, IBinder binder)
        {
            service = ((RadioService.LocalBinder)binder).service();
            service.setUi(MainActivity.this);
            service.audio().setVolume(prefs.volume() / 100.0F);
            service.audio().setMicLevel(prefs.micLevel() / 100.0F);
            service.client().setAudio(!prefs.muted(), true);
            if (tab == 2)
                service.client().setMonitor(true);
            changed();
        }

        public void onServiceDisconnected(ComponentName n)
        {
            service = null;
        }
    };

    private void bindRadio()
    {
        if (!bound)
            bound = bindService(new Intent(this, RadioService.class), connection, Context.BIND_AUTO_CREATE);
    }

    /** the person disconnects the app from OpenMT Radio (the pairing stays), or connects it again */
    private void toggleLink()
    {
        String link = service == null ? "offline" : service.link();
        if ("offline".equals(link) || "failed".equals(link))
        {
            prefs.setBool("linkOff", false);
            startRadio();
        }
        else
        {
            prefs.setBool("linkOff", true);
            if (service != null)
                service.client().pttUp();
            startService(new Intent(this, RadioService.class).setAction(RadioService.ACTION_STOP));
        }
        changed();
    }

    private void startRadio()
    {
        Intent i = new Intent(this, RadioService.class).setAction(RadioService.ACTION_START);
        startForegroundService(i);
        bindRadio();
    }

    /* ------------------------------------------------------------------------------------------------ small helpers */

    private int dp(int v)
    {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView text(String s, float sp, boolean dim)
    {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        if (dim)
            t.setTextColor(Color.rgb(139, 152, 168));
        t.setPadding(0, dp(2), 0, dp(2));
        return t;
    }

    private Button button(String s, View.OnClickListener l)
    {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setOnClickListener(l);
        return b;
    }

    private EditText edit(String hint, int inputType)
    {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setInputType(inputType);
        e.setSingleLine(true);
        return e;
    }

    private LinearLayout vbox()
    {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    private LinearLayout row()
    {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        return l;
    }

    private LinearLayout.LayoutParams weight()
    {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0F);
    }

    private ScrollView scroll(View inner)
    {
        ScrollView s = new ScrollView(this);
        s.addView(inner);
        return s;
    }

    private void ui(Runnable r)
    {
        handler.post(r);
    }

    /* ------------------------------------------------------------------------------------------------ layout */

    private void buildUi()
    {
        root = vbox();
        root.setPadding(dp(10), dp(10), dp(10), dp(6));

        linkLine = text("offline", 14, true);
        root.addView(linkLine);

        tabs = row();
        String[] names = { "Radio", "Settings", "Monitor", "Log" };
        for (int i = 0; i < 4; i++)
        {
            final int idx = i;
            tabButtons[i] = button(names[i], v -> selectTab(idx));
            tabs.addView(tabButtons[i], weight());
        }
        root.addView(tabs);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.0F));

        pairPanel = buildPairPanel();
        panels[0] = buildRadioPanel();
        panels[1] = buildSettingsPanel();
        panels[2] = buildMonitorPanel();
        panels[3] = buildLogPanel();
        content.addView(pairPanel);
        for (View p : panels)
            content.addView(p);

        setContentView(root);
        selectTab(0);
    }

    private View buildPairPanel()
    {
        LinearLayout p = vbox();
        p.setPadding(0, dp(10), 0, 0);
        p.addView(text("Pair with OpenMT Radio", 20, false));
        p.addView(text("In OpenMT Radio tick \"Allow remote control\" and enter here the address of that computer and the token it shows.", 14, true));
        pairHost = edit("Address of the computer (for example 192.168.1.20)", InputType.TYPE_TEXT_VARIATION_URI | InputType.TYPE_CLASS_TEXT);
        pairPort = edit("Port", InputType.TYPE_CLASS_NUMBER);
        pairPort.setText("18060");
        pairToken = edit("Token (ABCD-EFGH-JKLM-NPQR-STUV-WXYZ)", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                         | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        p.addView(pairHost);
        p.addView(pairPort);
        p.addView(pairToken);
        p.addView(button("Pair", v -> pair()));
        pairErr = text("", 14, false);
        pairErr.setTextColor(Color.rgb(239, 91, 91));
        p.addView(pairErr);
        p.addView(text("Anyone who has the token can transmit through your radio. Use this on your own network or over a VPN.", 13, true));
        return scroll(p);
    }

    private View buildRadioPanel()
    {
        LinearLayout p = vbox();

        modeLine = text("-", 20, false);
        statusLine = text("-", 15, true);
        heardLine = text("", 15, true);
        callLine = text("", 15, false);
        p.addView(modeLine);
        p.addView(statusLine);
        p.addView(heardLine);
        p.addView(callLine);

        linkBtn = button("Disconnect from OpenMT Radio", v -> toggleLink());
        p.addView(linkBtn);
        LinearLayout r1 = row();
        connectBtn = button("Connect radio", v -> cmd("connect", null, 60000));
        disconnectBtn = button("Disconnect", v -> { if (service != null) service.client().pttUp(); cmd("disconnect", null, 30000); });
        r1.addView(connectBtn, weight());
        r1.addView(disconnectBtn, weight());
        p.addView(r1);
        LinearLayout r2 = row();
        releaseBtn = button("Release modem", v -> cmd("release_modem", null, 30000));
        dongleBtn = button("Open dongle", v -> cmd("open_dongle", null, 30000));
        r2.addView(releaseBtn, weight());
        r2.addView(dongleBtn, weight());
        p.addView(r2);
        cmdErr = text("", 14, false);
        cmdErr.setTextColor(Color.rgb(239, 91, 91));
        p.addView(cmdErr);

        pttBtn = new Button(this);
        pttBtn.setText("PUSH TO TALK");
        pttBtn.setTextSize(22);
        pttBtn.setEnabled(false);
        pttBtn.setOnTouchListener((v, e) ->
        {
            if (service == null || !v.isEnabled())
                return false;
            int a = e.getActionMasked();
            if (latch.isChecked())
            {
                if (a == MotionEvent.ACTION_DOWN)
                {
                    if (service.holding())
                        service.client().pttUp();
                    else
                        service.client().pttDown();
                }
            }
            else if (a == MotionEvent.ACTION_DOWN)
                service.client().pttDown();
            else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL)
                service.client().pttUp();
            return true;
        });
        p.addView(pttBtn, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(130)));
        latch = new CheckBox(this);
        latch.setText("Tap to start and stop (instead of holding)");
        latch.setChecked(prefs.tapToggle());
        latch.setOnCheckedChangeListener((b, on) -> prefs.setBool("tapToggle", on));
        p.addView(latch);
        volKey = new CheckBox(this);
        volKey.setText("Volume-down key is push to talk (while the app is open)");
        volKey.setChecked(prefs.volumeKeyPtt());
        volKey.setOnCheckedChangeListener((b, on) -> prefs.setBool("volKeyPtt", on));
        p.addView(volKey);
        pttNote = text("", 13, true);
        p.addView(pttNote);

        muteBtn = button("Mute audio", v ->
        {
            boolean mute = !prefs.muted();
            prefs.setBool("muted", mute);
            if (service != null)
                service.client().setAudio(!mute, true);
            changed();
        });
        p.addView(muteBtn);
        p.addView(text("Volume on this phone", 13, true));
        SeekBar vol = new SeekBar(this);
        vol.setMax(300);
        vol.setProgress(prefs.volume());
        vol.setOnSeekBarChangeListener(new Seek()
        {
            void onValue(int v)
            {
                prefs.setInt("volume", v);
                if (service != null)
                    service.audio().setVolume(v / 100.0F);
            }
        });
        p.addView(vol);
        p.addView(text("Microphone level of this phone", 13, true));
        SeekBar mic = new SeekBar(this);
        mic.setMax(400);
        mic.setProgress(prefs.micLevel());
        mic.setOnSeekBarChangeListener(new Seek()
        {
            void onValue(int v)
            {
                prefs.setInt("micLevel", v);
                if (service != null)
                    service.audio().setMicLevel(v / 100.0F);
            }
        });
        p.addView(mic);
        audioInfo = text("", 12, true);
        p.addView(audioInfo);

        p.addView(text("DTMF", 13, true));
        GridLayout g = new GridLayout(this);
        g.setColumnCount(4);
        String[] keys = { "1", "2", "3", "A", "4", "5", "6", "B", "7", "8", "9", "C", "*", "0", "#", "D" };
        for (final String k : keys)
        {
            Button b = button(k, v -> cmd("dtmf", Json.map("digit", k), 5000));
            GridLayout.LayoutParams lp = new GridLayout.LayoutParams(GridLayout.spec(GridLayout.UNDEFINED, 1.0F), GridLayout.spec(GridLayout.UNDEFINED, 1.0F));
            lp.width = 0;
            g.addView(b, lp);
        }
        p.addView(g);

        LinearLayout m = vbox();
        msgTo = edit("Message to (M17)", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS);
        msgText = edit("Text", InputType.TYPE_CLASS_TEXT);
        metaText = edit("Text sent with every transmission (M17)", InputType.TYPE_CLASS_TEXT);
        m.addView(msgTo);
        m.addView(msgText);
        m.addView(button("Send message", v ->
        {
            cmd("send_message", Json.map("to", msgTo.getText().toString(), "text", msgText.getText().toString()), 10000);
            msgText.setText("");
        }));
        m.addView(metaText);
        m.addView(button("Set", v -> cmd("set_meta", Json.map("text", metaText.getText().toString()), 10000)));
        msgCard = m;
        p.addView(msgCard);

        p.addView(button("Forget this radio", v -> new AlertDialog.Builder(this)
            .setTitle("Forget this radio?")
            .setMessage("The app disconnects and must be paired again.")
            .setPositiveButton("Forget", (d, w) -> forget())
            .setNegativeButton("Cancel", null)
            .show()));
        return scroll(p);
    }

    private abstract class Seek implements SeekBar.OnSeekBarChangeListener
    {
        abstract void onValue(int v);

        public void onProgressChanged(SeekBar s, int v, boolean fromUser)
        {
            if (fromUser)
                onValue(v);
        }

        public void onStartTrackingTouch(SeekBar s)
        {
        }

        public void onStopTrackingTouch(SeekBar s)
        {
        }
    }

    private View buildSettingsPanel()
    {
        LinearLayout p = vbox();
        p.addView(text("Settings other than the M17 texts and the radio's own levels can only change while the radio is disconnected.", 13, true));
        LinearLayout r = row();
        r.addView(button("Save changes", v -> saveSettings()), weight());
        r.addView(button("Reload", v -> loadSettings()), weight());
        p.addView(r);
        setErr = text("", 14, false);
        p.addView(setErr);
        setForm = vbox();
        p.addView(setForm);
        return scroll(p);
    }

    private View buildMonitorPanel()
    {
        LinearLayout p = vbox();
        levels = new LevelsView(this);
        p.addView(levels, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(110)));
        monText = text("waiting ...", 14, false);
        monText.setTypeface(Typeface.MONOSPACE);
        p.addView(monText);
        return scroll(p);
    }

    private View buildLogPanel()
    {
        logView = text("", 11, false);
        logView.setTypeface(Typeface.MONOSPACE);
        logScroll = scroll(logView);
        return logScroll;
    }

    private void selectTab(int i)
    {
        tab = i;
        for (int k = 0; k < 4; k++)
        {
            panels[k].setVisibility(!paired() ? View.GONE : (k == i ? View.VISIBLE : View.GONE));
            tabButtons[k].setEnabled(k != i);
        }
        if (service != null)
            service.client().setMonitor(i == 2);
        if (i == 1 && fields.isEmpty())
            loadSettings();
        changed();
    }

    private boolean paired()
    {
        return prefs.paired();
    }

    private void showMode()
    {
        boolean p = paired();
        pairPanel.setVisibility(p ? View.GONE : View.VISIBLE);
        tabs.setVisibility(p ? View.VISIBLE : View.GONE);
        selectTab(tab);
    }

    /* ------------------------------------------------------------------------------------------------ pairing */

    private void pair()
    {
        final String host = pairHost.getText().toString().trim();
        final String token = RemoteConnection.normalizeToken(pairToken.getText().toString());
        int port;
        try
        {
            port = Integer.parseInt(pairPort.getText().toString().trim());
        }
        catch (NumberFormatException e)
        {
            port = 0;
        }
        final int fport = port;
        if (host.length() == 0 || port < 1 || port > 65535 || token.length() != 24)
        {
            pairErr.setText("An address, a port and the 24 characters of the token are needed.");
            return;
        }
        pairErr.setText("Contacting the radio ...");
        new Thread(() ->
        {
            try
            {
                final String fp = RemoteConnection.fetchFingerprint(host, fport, 5000);
                ui(() -> askTrust(host, fport, token, fp));
            }
            catch (Exception e)
            {
                ui(() -> pairErr.setText("The radio could not be reached: " + e.getMessage()));
            }
        }, "openmt-pair").start();
    }

    private void askTrust(final String host, final int port, final String token, final String fp)
    {
        pairErr.setText("");
        new AlertDialog.Builder(this)
            .setTitle("Is this your radio?")
            .setMessage("The radio's certificate fingerprint (SHA-256) is\n\n" + fp + "\n\nOpenMT Radio shows the same fingerprint in its Remote control box. "
                        + "Trust it only if they are the same.")
            .setPositiveButton("Trust and pair", (d, w) ->
            {
                prefs.pair(host, port, token, fp);
                prefs.setBool("linkOff", false);
                showMode();
                startRadio();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    private void forget()
    {
        startService(new Intent(this, RadioService.class).setAction(RadioService.ACTION_STOP));
        prefs.forget();
        fields.clear();
        setForm.removeAllViews();
        showMode();
        linkLine.setText("offline");
    }

    /* ------------------------------------------------------------------------------------------------ commands */

    private void cmd(String name, Map<String, Object> args, long timeoutMs)
    {
        if (service == null)
            return;
        cmdErr.setText("");
        service.client().command(name, args, timeoutMs, (ok, result, error) -> ui(() -> cmdErr.setText(ok ? "" : error)));
    }

    /* ------------------------------------------------------------------------------------------------ the push to talk keys */

    @Override
    public boolean onKeyDown(int code, KeyEvent e)
    {
        if (code == KeyEvent.KEYCODE_VOLUME_DOWN && prefs.volumeKeyPtt() && service != null && tab == 0)
        {
            if (e.getRepeatCount() == 0)
                service.client().pttDown();
            return true;
        }
        return super.onKeyDown(code, e);
    }

    @Override
    public boolean onKeyUp(int code, KeyEvent e)
    {
        if (code == KeyEvent.KEYCODE_VOLUME_DOWN && prefs.volumeKeyPtt() && service != null)
        {
            service.client().pttUp();
            return true;
        }
        return super.onKeyUp(code, e);
    }

    /* ------------------------------------------------------------------------------------------------ showing the state */

    @Override
    public void changed()
    {
        if (refreshPending)
            return;
        refreshPending = true;
        handler.postDelayed(() ->
        {
            refreshPending = false;
            refresh();
        }, 100);
    }

    private void refresh()
    {
        RadioService s = service;
        if (s == null || !paired())
            return;

        String link = s.link();
        String lm = s.linkMessage();
        linkLine.setText(link + (lm.length() > 0 ? ": " + lm : ""));
        linkBtn.setText("offline".equals(link) || "failed".equals(link) ? "Connect to OpenMT Radio" : "Disconnect from OpenMT Radio");
        linkLine.setTextColor("online".equals(link) ? Color.rgb(62, 207, 142) : "failed".equals(link) ? Color.rgb(239, 91, 91) : Color.rgb(245, 185, 66));

        Map<String, Object> st = s.state();
        boolean online = "online".equals(link) && st != null;
        boolean connected = online && Json.bool(st, "connected", false);

        modeLine.setText(online ? Json.str(st, "modeLabel", "") + (connected ? "  (connected)" : "  (not connected)") : "-");
        statusLine.setText(online ? Json.str(st, "status", "") : "");
        heardLine.setText(online ? Json.str(st, "heard", "") : "");
        callLine.setText(s.callText());
        connectBtn.setEnabled(online && !connected);
        disconnectBtn.setEnabled(connected);
        releaseBtn.setEnabled(online);
        dongleBtn.setEnabled(online && !connected);
        pttBtn.setEnabled(connected);
        boolean holding = s.holding();
        pttBtn.setText(holding ? "TRANSMITTING" : "PUSH TO TALK");
        pttBtn.setBackgroundColor(holding ? Color.rgb(200, 60, 60) : Color.rgb(50, 62, 80));
        String note = s.pttMessage();
        if (note.length() == 0)
            note = s.audio().micError();
        pttNote.setText(note);
        muteBtn.setText(prefs.muted() ? "Unmute audio" : "Mute audio");
        audioInfo.setText(prefs.muted() ? "audio muted" : "audio on");
        msgCard.setVisibility(online && (Json.bool(st, "messages", false) || (int)Json.num(st, "mode", -1) == 5) ? View.VISIBLE : View.GONE);
        for (Field f : fields)
            f.view.setEnabled(!connected || LIVE.contains("," + f.key + ","));

        if (tab == 2)
            showMonitor(s.monitor());
        if (tab == 3)
        {
            boolean stick = logScroll.getScrollY() + logScroll.getHeight() >= logView.getHeight() - dp(30);
            logView.setText(s.logText());
            if (stick)
                logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
        }
    }

    private void showMonitor(Map<String, Object> m)
    {
        if (m == null)
        {
            monText.setText("waiting ...");
            return;
        }
        StringBuilder sb = new StringBuilder();
        sb.append(Json.bool(m, "active", false) ? "Signal: " : "No signal: ")
          .append("iq".equals(Json.str(m, "source", "")) ? "IQ from the SDR" : "samples from the modem");
        if (Json.num(m, "silentMs", 0) > 0)
            sb.append(String.format("  (no data for %.1f s)", Json.num(m, "silentMs", 0) / 1000.0));
        sb.append('\n');
        String[][] rows = { { "RMS", "rms" }, { "Peak", "peak" }, { "Clipped", "clipped" }, { "Baud", "baud" }, { "Outer Hz", "outerHz" }, { "Nominal Hz", "nominalHz" },
            { "Outer %", "outerPercent" }, { "Spread", "spread" }, { "Blur", "blur" }, { "Sync locked", "syncLocked" }, { "Sync hits", "syncHits" },
            { "Inverted", "inverted" }, { "Averaged s", "averagedSeconds" }, { "IQ rate", "iqRate" }, { "IQ frames", "iqFrames" }, { "IQ lost", "iqLost" },
            { "IQ overload", "iqOverload" } };
        for (String[] r : rows)
            if (m.containsKey(r[1]))
                sb.append(String.format("%-14s %s\n", r[0], valueText(m.get(r[1]))));
        monText.setText(sb.toString());
        levels.set(m);
    }

    private static String valueText(Object v)
    {
        if (v instanceof Double)
        {
            double d = (Double)v;
            return d == Math.floor(d) ? String.valueOf((long)d) : String.format("%.2f", d);
        }
        return String.valueOf(v);
    }

    /** the four symbol levels as lines, and the centre */
    private static final class LevelsView extends View
    {
        private double[] lv;
        private double centre;
        private final Paint paint = new Paint();

        LevelsView(Context c)
        {
            super(c);
            paint.setStrokeWidth(6);
            paint.setTextSize(28);
        }

        void set(Map<String, Object> m)
        {
            List<Object> l = Json.arr(m.get("levels"));
            if (Json.bool(m, "levelsValid", false) && l != null && l.size() == 4)
            {
                lv = new double[4];
                for (int i = 0; i < 4; i++)
                    lv[i] = l.get(i) instanceof Double ? (Double)l.get(i) : 0.0;
                centre = Json.num(m, "centre", 0.0);
            }
            else
                lv = null;
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c)
        {
            int w = getWidth(), h = getHeight();
            c.drawColor(Color.rgb(11, 15, 20));
            if (lv == null)
            {
                paint.setColor(Color.rgb(139, 152, 168));
                c.drawText("no symbol levels yet", 20, h / 2f, paint);
                return;
            }
            double mx = 1.0;
            for (double v : lv)
                mx = Math.max(mx, Math.abs(v));
            paint.setColor(Color.rgb(44, 54, 68));
            c.drawLine(0, h / 2f, w, h / 2f, paint);
            paint.setColor(Color.rgb(77, 163, 255));
            for (double v : lv)
            {
                float y = (float)(h / 2.0 - (v / mx) * (h / 2.0 - 8));
                c.drawLine(40, y, w - 40, y, paint);
            }
            paint.setColor(Color.rgb(245, 185, 66));
            float yc = (float)(h / 2.0 - (centre / mx) * (h / 2.0 - 8));
            c.drawLine(0, yc, w, yc, paint);
        }
    }

    /* ------------------------------------------------------------------------------------------------ settings */

    private static String valueString(Object v)
    {
        if (v instanceof Double)
        {
            double d = (Double)v;
            return d == Math.floor(d) && Math.abs(d) < 9.0e15 ? String.valueOf((long)d) : String.valueOf(d);
        }
        return v == null ? "" : String.valueOf(v);
    }

    private void loadSettings()
    {
        if (service == null)
            return;
        setErr.setText("");
        final Object[] got = new Object[2];
        service.client().command("get_options", null, 10000, (ok, result, error) ->
        {
            got[0] = ok ? result : null;
            service.client().command("get_settings", null, 10000, (ok2, result2, error2) ->
            {
                got[1] = ok2 ? result2 : null;
                final Map<String, Object> options = Json.obj(got[0]);
                final Map<String, Object> settings = ok2 && result2 != null ? Json.obj(result2.get("settings")) : null;
                ui(() ->
                {
                    if (settings == null)
                        setErr.setText(ok2 ? "no settings" : error2);
                    else
                        buildSettings(options, settings);
                });
            });
        });
    }

    private void buildSettings(Map<String, Object> options, Map<String, Object> settings)
    {
        currentSettings = settings;
        setForm.removeAllViews();
        fields.clear();

        Map<String, List<String>> groups = new LinkedHashMap<String, List<String>>();
        for (String[] s : SECTIONS)
            groups.put(s[0], new ArrayList<String>());
        groups.put("Other", new ArrayList<String>());
        for (String key : new TreeMap<String, Object>(settings).keySet())
        {
            String g = "Other";
            for (String[] s : SECTIONS)
                if (Pattern.matches(s[1], key))
                {
                    g = s[0];
                    break;
                }
            groups.get(g).add(key);
        }

        Map<String, Object> choices = options == null ? null : Json.obj(options.get("choices"));
        List<Object> modes = options == null ? null : Json.arr(options.get("modes"));

        for (Map.Entry<String, List<String>> g : groups.entrySet())
        {
            if (g.getValue().isEmpty())
                continue;
            TextView head = text(g.getKey(), 17, false);
            head.setPadding(0, dp(14), 0, dp(2));
            head.setTypeface(Typeface.DEFAULT_BOLD);
            setForm.addView(head);

            for (String key : g.getValue())
            {
                Object v = settings.get(key);
                Field f = new Field();
                f.key = key;
                f.original = valueString(v);

                List<String> labels = null;
                if ("mode".equals(key) && modes != null)
                {
                    labels = new ArrayList<String>();
                    f.values = new ArrayList<String>();
                    for (Object o : modes)
                    {
                        Map<String, Object> m = Json.obj(o);
                        f.values.add(Json.str(m, "id", ""));
                        labels.add(Json.str(m, "label", "") + (Json.bool(m, "available", true) ? "" : " (not available)"));
                    }
                }
                else if (choices != null && Json.arr(choices.get(key)) != null)
                {
                    labels = new ArrayList<String>();
                    f.values = new ArrayList<String>();
                    for (Object o : Json.arr(choices.get(key)))
                    {
                        Map<String, Object> m = Json.obj(o);
                        f.values.add(Json.str(m, "value", ""));
                        labels.add(Json.str(m, "label", ""));
                    }
                }

                if (v instanceof Boolean)
                {
                    CheckBox cb = new CheckBox(this);
                    cb.setText(key);
                    cb.setChecked((Boolean)v);
                    f.type = 'b';
                    f.original = ((Boolean)v) ? "1" : "0";
                    f.view = cb;
                    setForm.addView(cb);
                }
                else
                {
                    setForm.addView(text(key, 13, true));
                    if (labels != null)
                    {
                        Spinner sp = new Spinner(this);
                        if (!f.values.contains(f.original))
                        {
                            f.values.add(f.original);
                            labels.add(f.original);
                        }
                        sp.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_spinner_dropdown_item, labels));
                        sp.setSelection(f.values.indexOf(f.original));
                        f.type = 'c';
                        f.view = sp;
                    }
                    else
                    {
                        boolean number = v instanceof Double;
                        EditText e = edit(key, number ? (InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED | InputType.TYPE_NUMBER_FLAG_DECIMAL)
                                                      : InputType.TYPE_CLASS_TEXT);
                        e.setText(f.original);
                        f.type = number ? 'n' : 's';
                        f.view = e;
                    }
                    setForm.addView(f.view);
                }
                fields.add(f);
            }
        }
        refresh();
    }

    private void saveSettings()
    {
        if (service == null)
            return;
        Map<String, Object> changes = new LinkedHashMap<String, Object>();
        for (Field f : fields)
        {
            if (!f.view.isEnabled())
                continue;
            String now;
            if (f.type == 'b')
                now = ((CheckBox)f.view).isChecked() ? "1" : "0";
            else if (f.type == 'c')
            {
                int i = ((Spinner)f.view).getSelectedItemPosition();
                now = i >= 0 && i < f.values.size() ? f.values.get(i) : f.original;
            }
            else
                now = ((EditText)f.view).getText().toString();
            if (now.equals(f.original))
                continue;
            if (f.type == 'b')
                changes.put(f.key, Boolean.valueOf(now.equals("1")));
            else if (f.type == 'n' || (f.type == 'c' && currentSettings.get(f.key) instanceof Double))
            {
                try
                {
                    changes.put(f.key, Double.valueOf(now));
                }
                catch (NumberFormatException e)
                {
                    setErr.setText(f.key + " needs a number");
                    return;
                }
            }
            else
                changes.put(f.key, now);
        }
        if (changes.isEmpty())
        {
            setErr.setText("Nothing changed.");
            return;
        }
        setErr.setText("saving ...");
        service.client().command("set_settings", Json.map("settings", changes), 10000, (ok, result, error) ->
        {
            ui(() ->
            {
                setErr.setText(ok ? "Saved." : error);
                if (ok)
                    loadSettings();
            });
        });
    }
}
