package org.openmt.radio.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** A small JSON reader and writer (no libraries, so that the protocol code also runs on a plain JVM).
 *  Values are Map (object), List (array), String, Double (every number), Boolean or null. */
public final class Json
{
    private Json()
    {
    }

    /* ------------------------------------------------------------------------------------------ helpers */

    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Object o)
    {
        return o instanceof Map ? (Map<String, Object>)o : null;
    }

    @SuppressWarnings("unchecked")
    public static List<Object> arr(Object o)
    {
        return o instanceof List ? (List<Object>)o : null;
    }

    public static String str(Map<String, Object> m, String key, String def)
    {
        Object v = m == null ? null : m.get(key);
        if (v instanceof String)
            return (String)v;
        if (v instanceof Double)
            return dump(v);
        return def;
    }

    public static double num(Map<String, Object> m, String key, double def)
    {
        Object v = m == null ? null : m.get(key);
        return v instanceof Double ? (Double)v : def;
    }

    public static boolean bool(Map<String, Object> m, String key, boolean def)
    {
        Object v = m == null ? null : m.get(key);
        return v instanceof Boolean ? (Boolean)v : def;
    }

    /** {"a":1,"b":"x"}: Json.map("a", 1, "b", "x") */
    public static Map<String, Object> map(Object... kv)
    {
        Map<String, Object> m = new LinkedHashMap<String, Object>();
        for (int i = 0; i + 1 < kv.length; i += 2)
        {
            Object v = kv[i + 1];
            if (v instanceof Integer)
                v = Double.valueOf(((Integer)v).doubleValue());
            else if (v instanceof Long)
                v = Double.valueOf(((Long)v).doubleValue());
            m.put((String)kv[i], v);
        }
        return m;
    }

    /* ------------------------------------------------------------------------------------------ writing */

    public static String dump(Object o)
    {
        StringBuilder sb = new StringBuilder();
        write(sb, o);
        return sb.toString();
    }

    private static void write(StringBuilder sb, Object o)
    {
        if (o == null)
            sb.append("null");
        else if (o instanceof Boolean)
            sb.append(((Boolean)o) ? "true" : "false");
        else if (o instanceof Number)
        {
            double d = ((Number)o).doubleValue();
            if (Double.isNaN(d) || Double.isInfinite(d))
                sb.append("null");
            else if (d == Math.floor(d) && Math.abs(d) < 9.0e15)
                sb.append((long)d);
            else
                sb.append(d);
        }
        else if (o instanceof String)
            quote(sb, (String)o);
        else if (o instanceof Map)
        {
            boolean first = true;
            sb.append('{');
            for (Map.Entry<?, ?> e : ((Map<?, ?>)o).entrySet())
            {
                if (!first)
                    sb.append(',');
                first = false;
                quote(sb, String.valueOf(e.getKey()));
                sb.append(':');
                write(sb, e.getValue());
            }
            sb.append('}');
        }
        else if (o instanceof List)
        {
            boolean first = true;
            sb.append('[');
            for (Object v : (List<?>)o)
            {
                if (!first)
                    sb.append(',');
                first = false;
                write(sb, v);
            }
            sb.append(']');
        }
        else
            quote(sb, o.toString());
    }

    private static void quote(StringBuilder sb, String s)
    {
        sb.append('"');
        for (int i = 0; i < s.length(); i++)
        {
            char c = s.charAt(i);
            switch (c)
            {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                case '\b': sb.append("\\b"); break;
                case '\f': sb.append("\\f"); break;
                default:
                    if (c < 0x20)
                        sb.append(String.format("\\u%04x", (int)c));
                    else
                        sb.append(c);
            }
        }
        sb.append('"');
    }

    /* ------------------------------------------------------------------------------------------ reading */

    public static Object parse(String text)
    {
        Reader r = new Reader(text);
        Object v = r.value(0);
        r.skip();
        if (r.pos != text.length())
            throw new IllegalArgumentException("trailing characters at " + r.pos);
        return v;
    }

    private static final class Reader
    {
        final String s;
        int pos;

        Reader(String s)
        {
            this.s = s;
        }

        void skip()
        {
            while (pos < s.length() && " \t\r\n".indexOf(s.charAt(pos)) >= 0)
                pos++;
        }

        IllegalArgumentException fail(String what)
        {
            return new IllegalArgumentException(what + " at " + pos);
        }

        Object value(int depth)
        {
            if (depth > 16)
                throw fail("nested too deeply");
            skip();
            if (pos >= s.length())
                throw fail("value expected");
            char c = s.charAt(pos);
            if (c == '{')
            {
                pos++;
                Map<String, Object> m = new LinkedHashMap<String, Object>();
                skip();
                if (pos < s.length() && s.charAt(pos) == '}')
                {
                    pos++;
                    return m;
                }
                for (;;)
                {
                    skip();
                    String key = string();
                    skip();
                    if (pos >= s.length() || s.charAt(pos) != ':')
                        throw fail("':' expected");
                    pos++;
                    m.put(key, value(depth + 1));
                    skip();
                    if (pos < s.length() && s.charAt(pos) == ',')
                    {
                        pos++;
                        continue;
                    }
                    if (pos < s.length() && s.charAt(pos) == '}')
                    {
                        pos++;
                        return m;
                    }
                    throw fail("',' or '}' expected");
                }
            }
            if (c == '[')
            {
                pos++;
                List<Object> l = new ArrayList<Object>();
                skip();
                if (pos < s.length() && s.charAt(pos) == ']')
                {
                    pos++;
                    return l;
                }
                for (;;)
                {
                    l.add(value(depth + 1));
                    skip();
                    if (pos < s.length() && s.charAt(pos) == ',')
                    {
                        pos++;
                        continue;
                    }
                    if (pos < s.length() && s.charAt(pos) == ']')
                    {
                        pos++;
                        return l;
                    }
                    throw fail("',' or ']' expected");
                }
            }
            if (c == '"')
                return string();
            if (s.startsWith("true", pos))
            {
                pos += 4;
                return Boolean.TRUE;
            }
            if (s.startsWith("false", pos))
            {
                pos += 5;
                return Boolean.FALSE;
            }
            if (s.startsWith("null", pos))
            {
                pos += 4;
                return null;
            }
            int start = pos;
            while (pos < s.length() && "+-0123456789.eE".indexOf(s.charAt(pos)) >= 0)
                pos++;
            if (start == pos)
                throw fail("unexpected character");
            try
            {
                return Double.valueOf(s.substring(start, pos));
            }
            catch (NumberFormatException e)
            {
                throw fail("bad number");
            }
        }

        String string()
        {
            if (pos >= s.length() || s.charAt(pos) != '"')
                throw fail("string expected");
            pos++;
            StringBuilder sb = new StringBuilder();
            while (pos < s.length())
            {
                char c = s.charAt(pos++);
                if (c == '"')
                    return sb.toString();
                if (c < 0x20)
                    throw fail("control character in string");
                if (c != '\\')
                {
                    sb.append(c);
                    continue;
                }
                if (pos >= s.length())
                    break;
                char e = s.charAt(pos++);
                switch (e)
                {
                    case '"':  sb.append('"'); break;
                    case '\\': sb.append('\\'); break;
                    case '/':  sb.append('/'); break;
                    case 'b':  sb.append('\b'); break;
                    case 'f':  sb.append('\f'); break;
                    case 'n':  sb.append('\n'); break;
                    case 'r':  sb.append('\r'); break;
                    case 't':  sb.append('\t'); break;
                    case 'u':
                        if (pos + 4 > s.length())
                            throw fail("short \\u escape");
                        try
                        {
                            sb.append((char)Integer.parseInt(s.substring(pos, pos + 4), 16));
                        }
                        catch (NumberFormatException x)
                        {
                            throw fail("bad \\u escape");
                        }
                        pos += 4;                   /* surrogate pairs come through as two chars, which is what a Java string wants */
                        break;
                    default:
                        throw fail("bad escape");
                }
            }
            throw fail("unterminated string");
        }
    }
}
