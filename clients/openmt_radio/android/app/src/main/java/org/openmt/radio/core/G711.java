package org.openmt.radio.core;

/** G.711 mu-law, the codec that halves the bandwidth of the audio. */
public final class G711
{
    private G711()
    {
    }

    public static byte encode(int sample)
    {
        int sign = 0;
        int s = sample;
        if (s < 0)
        {
            s = -s;
            sign = 0x80;
        }
        if (s > 32635)
            s = 32635;
        s += 0x84;
        int exp = 7;
        for (int mask = 0x4000; (s & mask) == 0 && exp > 0; mask >>= 1)
            exp--;
        int man = (s >> (exp + 3)) & 0x0F;
        return (byte)(~(sign | (exp << 4) | man));
    }

    public static short decode(byte code)
    {
        int c = ~code & 0xFF;
        int t = ((c & 0x0F) << 3) + 0x84;
        t <<= (c & 0x70) >> 4;
        return (short)((c & 0x80) != 0 ? (0x84 - t) : (t - 0x84));
    }
}
