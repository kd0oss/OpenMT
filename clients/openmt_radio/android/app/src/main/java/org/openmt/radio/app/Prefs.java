package org.openmt.radio.app;

import android.content.Context;
import android.content.SharedPreferences;

/** What the app remembers: the radio it is paired with (address, token, the pinned certificate) and the person's own levels. */
final class Prefs
{
    private final SharedPreferences p;

    Prefs(Context c)
    {
        p = c.getSharedPreferences("openmt", Context.MODE_PRIVATE);
    }

    String host()
    {
        return p.getString("host", "");
    }

    int port()
    {
        return p.getInt("port", 18060);
    }

    String token()
    {
        return p.getString("token", "");
    }

    String fingerprint()
    {
        return p.getString("fp", "");
    }

    boolean paired()
    {
        return host().length() > 0 && token().length() > 0 && fingerprint().length() > 0;
    }

    void pair(String host, int port, String token, String fingerprint)
    {
        p.edit().putString("host", host).putInt("port", port).putString("token", token).putString("fp", fingerprint).apply();
    }

    void forget()
    {
        p.edit().remove("host").remove("port").remove("token").remove("fp").apply();
    }

    int volume()
    {
        return p.getInt("volume", 100);
    }

    int micLevel()
    {
        return p.getInt("micLevel", 100);
    }

    /** the person chose to disconnect the app from the radio: the app stays disconnected until they connect again */
    boolean linkOff()
    {
        return p.getBoolean("linkOff", false);
    }

    boolean muted()
    {
        return p.getBoolean("muted", false);
    }

    boolean volumeKeyPtt()
    {
        return p.getBoolean("volKeyPtt", false);
    }

    boolean tapToggle()
    {
        return p.getBoolean("tapToggle", false);
    }

    void setInt(String key, int v)
    {
        p.edit().putInt(key, v).apply();
    }

    void setBool(String key, boolean v)
    {
        p.edit().putBoolean(key, v).apply();
    }
}
