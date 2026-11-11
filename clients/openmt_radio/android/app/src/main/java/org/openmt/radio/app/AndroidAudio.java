package org.openmt.radio.app;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.AudioTrack;
import android.media.MediaRecorder;
import android.os.Process;

import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

import org.openmt.radio.core.RadioClient;

/** The phone's speaker and microphone for RadioClient: the speech of the radio (8 kHz) goes through a small jitter buffer to an
 *  AudioTrack; the microphone is recorded while the key is down and cut into frames of 160 samples at 8 kHz.  The levels are the
 *  phone's own (the radio's settings are not touched). */
final class AndroidAudio implements RadioClient.Speaker, RadioClient.Microphone
{
    private static final int RATE = 8000;

    private final LinkedBlockingQueue<short[]> queue = new LinkedBlockingQueue<short[]>();
    private volatile float volume = 1.0F;
    private volatile float micLevel = 1.0F;
    private volatile boolean alive = true;
    private volatile RadioClient client;
    private final Thread player;

    private volatile Thread recorder;
    private volatile boolean recording;
    private volatile String micError = "";

    AndroidAudio()
    {
        player = new Thread(new Runnable()
        {
            public void run()
            {
                playLoop();
            }
        }, "openmt-speaker");
        player.setDaemon(true);
        player.start();
    }

    void setClient(RadioClient c)
    {
        client = c;
    }

    void setVolume(float v)
    {
        volume = v;
    }

    void setMicLevel(float v)
    {
        micLevel = v;
    }

    String micError()
    {
        return micError;
    }

    void release()
    {
        alive = false;
        stop();
        player.interrupt();
    }

    /* ------------------------------------------------------------------------------------------------ speaker */

    public void play(short[] pcm, int count)
    {
        short[] copy = new short[count];
        System.arraycopy(pcm, 0, copy, 0, count);
        if (queue.size() > 100)                            /* 2 s: the player is stuck, drop the oldest */
            queue.poll();
        queue.offer(copy);
    }

    public void clear()
    {
        queue.clear();
    }

    private void playLoop()
    {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);

        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack track = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build())
            .setAudioFormat(new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                .build())
            .setBufferSizeInBytes(Math.max(min, RATE))              /* half a second */
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build();

        boolean playing = false;
        long lastData = 0L;
        short[] silence = new short[RATE / 1000 * 160];             /* the 160 ms jitter buffer */

        try
        {
            while (alive)
            {
                short[] f;
                try
                {
                    f = queue.poll(200, TimeUnit.MILLISECONDS);
                }
                catch (InterruptedException e)
                {
                    break;
                }
                long now = System.currentTimeMillis();
                if (f == null)
                {
                    if (playing && now - lastData > 600L)
                    {
                        track.pause();
                        track.flush();
                        playing = false;
                    }
                    continue;
                }
                lastData = now;
                if (!playing)
                {
                    track.write(silence, 0, silence.length);
                    track.play();
                    playing = true;
                }
                float g = volume;
                short[] out = f;
                if (g != 1.0F)
                {
                    out = new short[f.length];
                    for (int i = 0; i < f.length; i++)
                    {
                        int v = Math.round(f[i] * g);
                        out[i] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                    }
                }
                track.write(out, 0, out.length);                    /* blocks until the track has room: this paces the playback */
            }
        }
        finally
        {
            try
            {
                track.stop();
            }
            catch (IllegalStateException ignored)
            {
            }
            track.release();
        }
    }

    /* ------------------------------------------------------------------------------------------------ microphone */

    public synchronized void start()
    {
        if (recorder != null)
            return;
        recording = true;
        micError = "";
        Thread t = new Thread(new Runnable()
        {
            public void run()
            {
                recordLoop();
            }
        }, "openmt-microphone");
        t.setDaemon(true);
        recorder = t;
        t.start();
    }

    public synchronized void stop()
    {
        recording = false;
        Thread t = recorder;
        recorder = null;
        if (t != null)
            t.interrupt();
    }

    private void recordLoop()
    {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO);

        int[] rates = { 8000, 16000, 48000, 44100 };
        AudioRecord rec = null;
        int rate = 0;

        for (int i = 0; i < rates.length && rec == null; i++)
        {
            int min = AudioRecord.getMinBufferSize(rates[i], AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0)
                continue;
            try
            {
                AudioRecord r = new AudioRecord(MediaRecorder.AudioSource.VOICE_COMMUNICATION, rates[i], AudioFormat.CHANNEL_IN_MONO,
                                                AudioFormat.ENCODING_PCM_16BIT, Math.max(min, rates[i] / 5));
                if (r.getState() == AudioRecord.STATE_INITIALIZED)
                {
                    rec = r;
                    rate = rates[i];
                }
                else
                    r.release();
            }
            catch (SecurityException e)
            {
                micError = "no permission for the microphone";
                return;
            }
            catch (IllegalArgumentException e)
            {
                /* try the next rate */
            }
        }
        if (rec == null)
        {
            micError = "the microphone could not be opened";
            return;
        }

        try
        {
            rec.startRecording();
            short[] in = new short[rate / 50];                       /* 20 ms */
            short[] out = new short[160];
            int fill = 0;
            int phase = 0;
            long acc = 0L;
            int cnt = 0;

            while (recording)
            {
                int n = 0;
                while (n < in.length && recording)
                {
                    int r = rec.read(in, n, in.length - n);
                    if (r <= 0)
                        break;
                    n += r;
                }
                if (n < in.length)
                    break;

                float g = micLevel;
                for (int i = 0; i < n; i++)
                {
                    acc += in[i];
                    cnt++;
                    phase += RATE;
                    if (phase >= rate)                              /* a box filter down to 8 kHz */
                    {
                        phase -= rate;
                        int v = Math.round((acc / (float)cnt) * g);
                        out[fill++] = (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                        acc = 0L;
                        cnt = 0;
                        if (fill == out.length)
                        {
                            RadioClient c = client;
                            if (c != null)
                                c.feedMic(out, 0, out.length);
                            fill = 0;
                        }
                    }
                }
            }
        }
        finally
        {
            try
            {
                rec.stop();
            }
            catch (IllegalStateException ignored)
            {
            }
            rec.release();
        }
    }
}
