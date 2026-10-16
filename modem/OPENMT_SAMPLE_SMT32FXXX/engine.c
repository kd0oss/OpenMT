/*
 *  engine.c - OpenMT SAMPLE-mode protocol and sample engine.
 *
 *  Portable: talks to the hardware only through hw.h.
 *
 *  Data flow
 *
 *    ADC --> engine_sample_tick() --> rx frame ring --> engine_poll() --> UART
 *    UART --> engine_poll() parser --> TX sample ring --> engine_sample_tick()
 *                                                           --> DAC + PTT
 *
 *  engine_sample_tick() runs in the 24 kHz timer interrupt, everything
 *  else runs in the main loop.  The two sides share only single
 *  producer / single consumer rings and a few flags.
 */

#include <string.h>
#include "config.h"

#include "hw.h"
#include "engine.h"

#define TX_MASK         (TX_BUF_SAMPLES - 1)
#define RX_FRAMES       4                   /* power of two              */
#define RX_FRAME_BYTES  (3 + SAMPLES_PER_FRAME * 2)

enum
{
    TX_IDLE = 0,                            /* PTT off                   */
    TX_PRIME,                               /* PTT on, filling the buffer */
    TX_PLAY                                 /* PTT on, audio going out   */
};

/* ------------------------------------------------------------------ */
/* State shared between interrupt and main loop                       */
/* ------------------------------------------------------------------ */
static int16_t           txBuf[TX_BUF_SAMPLES];
static volatile uint16_t txHead;            /* written by main           */
static volatile uint16_t txTail;            /* written by interrupt      */
static volatile uint8_t  txState;
static volatile uint8_t  txEot;

static int16_t           rxBuf[RX_FRAMES][SAMPLES_PER_FRAME];
static uint16_t          rxIdx;             /* interrupt only            */
static volatile uint32_t rxWrCount;         /* frames completed (irq)    */
static uint32_t          rxRdCount;         /* frames sent (main)        */

static volatile int32_t  rxGainQ8   = RX_GAIN_Q8;
static volatile uint8_t  rxInvert;
static volatile uint8_t  duplex;
static int32_t           rxDcQ12;

static volatile uint8_t  isrClip;
static volatile uint8_t  isrActivity;
static volatile uint8_t  isrUnderrun;
static uint32_t          stallCount;        /* silent samples since underrun */
static uint8_t           stallAlign;        /* resuming after an underrun   */
static volatile uint32_t isrRxDropped;
static volatile uint32_t isrUnderrunCount;
static volatile uint32_t isrClipCount;

#if DEBUG_FRAMES
static volatile uint16_t dbgTxMin = 0xFFFFU;   /* lowest TX fill while playing */
static uint32_t          dbgClipSeen;       /* isrClipCount at last report */
static volatile uint32_t dbgPeak;           /* RX peak |q15| this window */
static volatile int32_t  dbgSum;            /* sum of (sample >> 4)      */
static volatile uint32_t dbgCount;
#endif

/* TX/RX alignment report (DMR slot timing).  The ADC and DAC share one
 * sample interrupt, so the RX sample being captured when a TX sample goes
 * out is known exactly.  The interrupt latches it, the main loop sends it. */
static uint32_t          playIdx;           /* TX samples played (irq)   */
static volatile uint8_t  tmPending;         /* latch at the next sample  */
static volatile uint8_t  tmReady;           /* latched, not yet sent     */
static volatile uint32_t tmSeq;             /* odd while being written   */
static volatile uint32_t tmK;
static volatile uint32_t tmR;

/* ------------------------------------------------------------------ */
/* Main loop only state                                               */
/* ------------------------------------------------------------------ */
static engine_stats_t    stats;
static bool              configured;
static bool              txInvert;
static uint32_t          lastDataMs;
static uint32_t          lastNakMs;
static uint32_t          lastHostMs;
static uint32_t          ledUntil[NUM_LEDS];
static uint32_t          lastUartErr;
static uint32_t          lastUnderrunSeen;
static uint32_t          lastClipSeen;
static uint32_t          lastDropSeen;

static const uint8_t ledPin[NUM_LEDS] =
{
    LED0_PIN, LED1_PIN, LED2_PIN, LED3_PIN,
    LED4_PIN, LED5_PIN, LED6_PIN, LED7_PIN
};

/* ------------------------------------------------------------------ */
/* Sample interrupt                                                   */
/* ------------------------------------------------------------------ */
static inline int16_t clamp16(int32_t v)
{
    if (v > 32767)
    {
        return 32767;
    }
    if (v < -32767)
    {
        return -32767;
    }
    return (int16_t)v;
}

static inline uint16_t txCount(void)
{
    return (uint16_t)((txHead - txTail) & TX_MASK);
}

#if TX_TEST_PATTERN
/* n = TX sample index within the downlink */
static int16_t testPattern(uint32_t n)
{
    uint32_t step = (n / 9600U) % 6U;               /* 400 ms steps      */
    uint32_t sym = (n / 5U) & 1U;                   /* symbol parity     */
    int32_t a = TX_TEST_AMPL;

    switch (step)
    {
    case 0:  return (int16_t)a;
    case 1:  return (int16_t)(a / 3);
    case 2:  return (int16_t)(-(a / 3));
    case 3:  return (int16_t)(-a);
    case 4:  return 0;
    default: return (int16_t)(sym ? -a : a);
    }
}
#endif

uint16_t engine_sample_tick(uint16_t adcRaw)
{
    int32_t  s;
    int32_t  dacv;
    int16_t  out = 0;
    uint8_t  st;
    uint32_t rxPos = rxWrCount * SAMPLES_PER_FRAME + rxIdx;   /* this sample's
                                                  index in the RX stream */

    /* ---------------- RX ---------------- */
    s = ((int32_t)adcRaw - ADC_MID) * (1 << RX_ADC_SHIFT);  /* 12 bit -> q15 */

    if (adcRaw <= 4U || adcRaw >= 4091U)
    {
        isrClip = 1;
        isrClipCount++;
    }

#if RX_DC_BLOCK
    rxDcQ12 += (((s << 12) - rxDcQ12) >> RX_DC_SHIFT);
    s -= (rxDcQ12 >> 12);
#endif

    if (rxInvert)
    {
        s = -s;
    }

    s = (s * rxGainQ8) >> 8;
    s = clamp16(s);

    if (!duplex && txState != TX_IDLE)
    {
        s = 0;                                      /* simplex: deaf on TX */
    }

    if (s > RX_ACTIVITY_LEVEL || s < -RX_ACTIVITY_LEVEL)
    {
        isrActivity = 1;
    }

#if DEBUG_FRAMES
    {
        uint32_t a = (uint32_t)(s < 0 ? -s : s);

        if (a > dbgPeak)
        {
            dbgPeak = a;
        }
        dbgSum += (s >> 4);
        dbgCount++;
    }
#endif

    if ((uint32_t)(rxWrCount - rxRdCount) >= RX_FRAMES)
    {
        /* main loop is not draining (host gone / UART full): drop */
        if (++rxIdx >= SAMPLES_PER_FRAME)
        {
            rxIdx = 0;
            isrRxDropped++;
        }
    }
    else
    {
        rxBuf[rxWrCount & (RX_FRAMES - 1)][rxIdx++] = (int16_t)s;
        if (rxIdx >= SAMPLES_PER_FRAME)
        {
            rxIdx = 0;
            rxWrCount++;
        }
    }

    /* ---------------- TX ---------------- */
    st = txState;

    if (st == TX_PRIME)
    {
        if (txEot ||
            (txCount() >= TX_PRIME_SAMPLES &&
             (!stallAlign || (stallCount % TX_CYCLE_SAMPLES) == 0U)))
        {
            st = TX_PLAY;
            txState = TX_PLAY;
            stallAlign = 0;
        }
        else if (stallAlign)
        {
            stallCount++;                           /* still silent       */
        }
    }

    if (st == TX_PLAY)
    {
        if (txCount() != 0)
        {
#if DEBUG_FRAMES
            {
                uint16_t fill = (uint16_t)txCount();

                if (fill < dbgTxMin)
                {
                    dbgTxMin = fill;
                }
            }
#endif
            out = txBuf[txTail];
            txTail = (uint16_t)((txTail + 1) & TX_MASK);
#if TX_TEST_PATTERN
            out = testPattern(playIdx);
#endif
            if (tmPending)
            {
                tmSeq++;                            /* odd: writing      */
                tmK = playIdx;
                tmR = rxPos;
                tmSeq++;
                tmReady = 1;
                tmPending = 0;
            }
            playIdx++;
#if TX_TIMING_REPORT
            if ((playIdx % TX_TIMING_REFRESH) == 0U)
            {
                tmPending = 1;                      /* re-anchor the host */
            }
#endif
        }
        else if (txEot)
        {
            /* everything the host sent has been played: key down */
            txState = TX_IDLE;
            hw_ptt(false);
        }
        else
        {
            /* ran dry: wait for more data before restarting audio */
            txState = TX_PRIME;
#if TX_STALL_CYCLE_ALIGN
            stallAlign = 1;                         /* resume on a cycle  */
            stallCount = 1U;                        /* this sample is silent */
#endif
            tmPending = 1;                          /* new alignment on resume */
            isrUnderrun = 1;
            isrUnderrunCount++;
        }
    }

    dacv = DAC_MID + TX_DC_OFFSET + (out >> TX_DAC_SHIFT);
    if (dacv < 0)
    {
        dacv = 0;
    }
    else if (dacv > 4095)
    {
        dacv = 4095;
    }
    return (uint16_t)dacv;
}

/* ------------------------------------------------------------------ */
/* Helpers (main loop)                                                */
/* ------------------------------------------------------------------ */
static void ledPulse(uint8_t n, uint32_t ms)
{
    ledUntil[n] = hw_millis() + ms;
}

static void sendSmall(uint8_t type)
{
    uint8_t f[3];

    f[0] = OMT_START;
    f[1] = 3;
    f[2] = type;
    hw_uart_tx(f, 3);
}

static void sendAck(void)
{
    sendSmall(OMT_ACK);
}

static void sendNak(uint8_t type, uint8_t err)
{
    uint8_t f[5];

    f[0] = OMT_START;
    f[1] = 5;
    f[2] = OMT_NAK;
    f[3] = type;
    f[4] = err;
    hw_uart_tx(f, 5);
}

static uint8_t freeSpaceNibble(void)
{
    uint32_t freeSamples = (uint32_t)(TX_BUF_SAMPLES - 1) - txCount();
    uint32_t frames = freeSamples / SAMPLES_PER_FRAME;

    return (uint8_t)(frames > 15U ? 15U : frames);
}

static void keyDown(void)
{
    /* used for timeouts and abort: drop whatever is queued */
    txState = TX_IDLE;
    txTail = txHead;
    hw_ptt(false);
}

static void requestEot(void)
{
    if (txState != TX_IDLE)
    {
        txEot = 1;                                  /* drain, then unkey */
    }
}

/* ------------------------------------------------------------------ */
/* Frame handlers                                                     */
/* ------------------------------------------------------------------ */
/* "NAME vX.Y.Z Mon DD YYYY id=IDCODE" as a DEBUG (0xC0) text frame, the same kind  */
/* the host already logs as "Modem rec data::".                           */
static void sendVersion(void)
{
    static const char text[] = FW_NAME " v" FW_VERSION " " __DATE__ " id=";
    static const char hex[] = "0123456789ABCDEF";
    uint8_t f[3 + sizeof(text) + 8];
    uint8_t n = 3;
    uint8_t i;
    uint32_t id = hw_chip_id();

    for (i = 0; text[i] != '\0'; i++)
    {
        f[n++] = (uint8_t)text[i];
    }
    for (i = 0; i < 8; i++)                         /* DBGMCU_IDCODE, hex */
    {
        f[n++] = (uint8_t)hex[(id >> (28 - 4 * i)) & 0xFU];
    }
    f[0] = OMT_START;
    f[1] = n;
    f[2] = OMT_DEBUG;
    hw_uart_tx(f, n);
}

static void handleSetConfig(const uint8_t *f, uint8_t len)
{
    if (len >= 5)
    {
        uint8_t flags = f[3];
        uint8_t rxLevel = f[4];
        int32_t g;

        duplex = (flags & 0x01U) ? 1 : 0;
        rxInvert = (flags & 0x02U) ? 1 : 0;
        txInvert = (flags & 0x04U) != 0;
        hw_set_ptt_invert((flags & 0x08U) != 0);

#if RX_USE_HOST_LEVEL
        g = ((int32_t)rxLevel * RX_GAIN_Q8) / RX_LEVEL_UNITY;
        if (g > 255 * 16)
        {
            g = 255 * 16;
        }
#else
        (void)rxLevel;
        g = RX_GAIN_Q8;
#endif
        rxGainQ8 = g;
    }
    bool first = !configured;

    configured = true;
    sendAck();
    if (first)
    {
        sendVersion();                              /* once per host link */
    }
}

static void handleSampData(const uint8_t *f, uint8_t len)
{
    uint8_t  txLevel;
    uint16_t n;
    uint16_t i;
    int32_t  g;
    uint16_t head;

    if (len < 4)
    {
        return;
    }

    txLevel = f[3];
    n = (uint16_t)((len - 4) / 2);
    stats.txFramesFromHost++;

    if (n > (uint16_t)((TX_BUF_SAMPLES - 1) - txCount()))
    {
        stats.txOverflow++;
        ledPulse(7, 500);
        if ((uint32_t)(hw_millis() - lastNakMs) >= 100U)
        {
            lastNakMs = hw_millis();
            sendNak(OMT_SAMP_DATA, OMT_ERR_NO_SPACE);
        }
        return;
    }

#if TX_USE_HOST_LEVEL
    /* level 0 means "not set": treat as unity rather than silence */
    g = ((int32_t)(txLevel ? txLevel : TX_LEVEL_UNITY) * TX_GAIN_Q8) / TX_LEVEL_UNITY;
#else
    (void)txLevel;
    g = TX_GAIN_Q8;
#endif

    head = txHead;
    for (i = 0; i < n; i++)
    {
        int32_t v = (int16_t)((uint16_t)f[4 + 2 * i] | ((uint16_t)f[5 + 2 * i] << 8));

        if (txInvert)
        {
            v = -v;
        }
        v = (v * g) >> 8;
        txBuf[head] = clamp16(v);
        head = (uint16_t)((head + 1) & TX_MASK);
    }
    txHead = head;
    txEot = 0;                                      /* more data: not over */
    lastDataMs = hw_millis();

    if (txState == TX_IDLE)
    {
        stats.keyUps++;
        playIdx = 0;                                /* TX sample 0 = first sent */
        tmReady = 0;
        stallAlign = 0;
        tmPending = TX_TIMING_REPORT;
        txState = TX_PRIME;
        hw_ptt(true);
    }
}

static void handleFrame(const uint8_t *f, uint8_t len)
{
    uint8_t type = f[2] & 0xF0U;
    uint8_t nib = f[2] & 0x0FU;

    lastHostMs = hw_millis();
    ledPulse(1, 30);

    switch (type)
    {
        case OMT_SAMP_EOT:                          /* 0x00: end of TX   */
        case 0x90U:
            requestEot();
            break;

        case OMT_GET_FW_VERSION:
            if (nib == 0U)
            {
                sendVersion();                      /* 61 03 D0          */
            }
            else if (nib == 1U)
            {
                /* 61 07 D1 'B' 'O' 'O' 'T': reboot into the ROM boot loader */
                if (BOOTLOADER_COMMAND && len >= 7U && f[3] == 'B' && f[4] == 'O' &&
                    f[5] == 'O' && f[6] == 'T')
                {
                    sendAck();
                    hw_enter_bootloader();
                }
                else
                {
                    sendNak(type | nib, 1U);
                }
            }
            break;

        case OMT_GET_STATUS:
            sendSmall((uint8_t)(OMT_GET_STATUS | freeSpaceNibble()));
            break;

        case OMT_SET_CONFIG:
            handleSetConfig(f, len);
            break;

        case OMT_SET_MODE:
            if (nib == 0x0FU)
            {
                requestEot();                       /* idle */
            }
            sendAck();
            break;

        case OMT_ADD_MODE:
        case OMT_DEL_MODE:
            sendAck();
            break;

        case OMT_SAMP_DATA:
            handleSampData(f, len);
            break;

        default:
            break;                                  /* BIT data etc.: n/a */
    }
}

static bool knownType(uint8_t t)
{
    switch (t & 0xF0U)
    {
        case 0x00U: case 0x10U: case 0x20U: case 0x30U:
        case 0x40U: case 0x50U: case 0x60U: case 0x70U:
        case 0x80U: case 0x90U: case 0xA0U: case 0xC0U: case 0xD0U:
        case 0xE0U: case 0xF0U:
            return true;
        default:
            return false;
    }
}

#if DEBUG_FRAMES
static uint32_t dbgSkipEvents;              /* runs of skipped bytes     */
static uint8_t  dbgSkipping;
static uint8_t  dbgPrevLen;                 /* last good frame           */
static uint8_t  dbgPrevType;
static uint8_t  dbgSk[6];                   /* first bytes of last run   */
static uint8_t  dbgPre[6];                  /* the 6 bytes just before   */
static uint16_t dbgRun;                     /* length of the current run */
static uint16_t dbgRunLast;                 /* length of the last run    */
static uint16_t dbgRunMax;
static uint16_t dbgMaxAvail;                /* deepest UART RX backlog   */
static uint32_t dbgMaxGap;                  /* longest gap between polls */
static uint32_t dbgLastPoll;

static void dbgNoteSkip(uint16_t avail)
{
    uint8_t i;

    dbgRun++;
    if (!dbgSkipping)
    {
        dbgSkipping = 1;
        dbgSkipEvents++;
        for (i = 0; i < 6; i++)
        {
            dbgSk[i] = (i < avail) ? hw_uart_rx_peek(i) : 0;
            dbgPre[i] = hw_uart_rx_peek((uint16_t)(0U - 6U + i));
        }
    }
}
#define DBG_SKIP(a)   dbgNoteSkip(a)
#define DBG_GOOD(l,t) do { if (dbgSkipping) { dbgRunLast = dbgRun; if (dbgRun > dbgRunMax) dbgRunMax = dbgRun; } dbgRun = 0; dbgSkipping = 0; dbgPrevLen = (l); dbgPrevType = (t); } while (0)
#else
#define DBG_SKIP(a)   ((void)0)
#define DBG_GOOD(l,t) ((void)0)
#endif

/* A sample frame whose bytes were damaged on the way in (the next header is not
 * where a frame of this length says it should be) is dropped and replaced by one
 * frame of silence, so the TX stream keeps its sample alignment. */
static void insertSilenceFrame(void)
{
    static uint8_t z[4U + 2U * SAMPLES_PER_FRAME];

    if (txState == TX_IDLE)
    {
        return;                                     /* not in a transmission */
    }
    stats.lostFrames++;
    z[0] = OMT_START;
    z[1] = (uint8_t)sizeof z;
    z[2] = OMT_SAMP_DATA;
    z[3] = TX_LEVEL_UNITY;
    handleSampData(z, (uint8_t)sizeof z);
}

static uint8_t  holding;
static uint32_t holdMs;
static uint8_t  resync;                     /* looking for a trustworthy header */
static uint32_t resyncMs;                   /* last time bytes were skipped    */
#define RESYNC_QUIET_MS 20U
static uint16_t resyncBytes;                /* bytes skipped in this hunt      */
static uint8_t  resyncFrame;                /* hunt began at a damaged frame   */

#define PARSE_HOLD_MS   8U


/* End of a hunt for the next frame: every 196 bytes that were skipped is
 * one sample frame the timeline has to keep, so replace them by silence. */
#define NOTE_SKIP() do { if (resyncBytes < 60000U) { resyncBytes++; } resync = 1U; resyncMs = hw_millis(); } while (0)

static void endResync(void)
{
    uint16_t n = (uint16_t)((resyncBytes + 98U) / 196U);

    if (resyncFrame && n == 0U)
    {
        n = 1U;                                     /* the damaged frame itself */
    }
    resyncFrame = 0U;
    resyncBytes = 0U;
    resync = 0U;
    while (n--)
    {
        insertSilenceFrame();
    }
}

/* The DMA ring head (NDTR) can be seen one byte ahead of the memory write,
 * so the newest byte may still hold its previous-lap value for an instant.
 * parseUart checks the byte after a frame the moment it arrives, which is
 * exactly that case: look again before declaring the frame damaged. */
static uint32_t dbgRecovered;               /* false alarms avoided      */
static uint8_t  dbgBadNext;                 /* byte seen after a bad frame */

static bool nextIsStart(uint16_t len)
{
    uint8_t k;
    volatile uint16_t spin;

    for (k = 0U; k < 4U; k++)
    {
        if (hw_uart_rx_peek(len) == OMT_START)
        {
            if (k != 0U)
            {
                dbgRecovered++;
            }
            return true;
        }
        for (spin = 0U; spin < 300U; spin++)
        {
        }
    }
    dbgBadNext = hw_uart_rx_peek(len);
    return false;
}

static void parseUart(void)
{
    uint8_t frame[255];
    uint16_t avail;

#if DEBUG_FRAMES
    {
        uint32_t nowMs = hw_millis();
        uint16_t a0 = hw_uart_rx_available();

        if (dbgLastPoll != 0U && (uint32_t)(nowMs - dbgLastPoll) > dbgMaxGap)
        {
            dbgMaxGap = (uint32_t)(nowMs - dbgLastPoll);
        }
        dbgLastPoll = nowMs;
        if (a0 > dbgMaxAvail)
        {
            dbgMaxAvail = a0;
        }
    }
#endif
    while ((avail = hw_uart_rx_available()) >= 1U)
    {
        uint8_t len;
        uint16_t i;
        bool    isSamp;

        if (hw_uart_rx_peek(0) != OMT_START)
        {
            DBG_SKIP(avail);
            hw_uart_rx_consume(1);
            stats.badBytes++;
            NOTE_SKIP();
            continue;
        }
        if (avail < 3U)
        {
            return;
        }
        len = hw_uart_rx_peek(1);
        if (len < 3U || !knownType(hw_uart_rx_peek(2)))
        {
            DBG_SKIP(avail);
            hw_uart_rx_consume(1);                  /* resync */
            stats.badBytes++;
            NOTE_SKIP();
            continue;
        }
        isSamp = (len == (uint8_t)(4U + 2U * SAMPLES_PER_FRAME)) &&
                 ((hw_uart_rx_peek(2) & 0xF0U) == OMT_SAMP_DATA);
        if (resync && (uint32_t)(hw_millis() - resyncMs) > RESYNC_QUIET_MS)
        {
            endResync();                            /* quiet: back to normal */
        }
        if (resync && len != 3U && len != (uint8_t)(4U + 2U * SAMPLES_PER_FRAME))
        {
            /* Hunting for the next frame after damage: a 0x61 inside sample
             * data followed by a plausible length and type is far more
             * likely than a real command, so only accept the two shapes
             * that make up the stream (196-byte frames, 3-byte requests). */
            DBG_SKIP(avail);
            hw_uart_rx_consume(1);
            stats.badBytes++;
            NOTE_SKIP();
            continue;
        }
        if (avail < len)
        {
            return;                                 /* wait for the rest */
        }
        if (isSamp)
        {
            if (avail == len)
            {
                /* The next header has not arrived yet, so this frame cannot
                 * be verified.  Wait for it (a frame follows every 4 ms),
                 * but not forever: the last frame of a stream has none. */
                if (!holding)
                {
                    holding = 1U;
                    holdMs = hw_millis();
                }
                if ((uint32_t)(hw_millis() - holdMs) < PARSE_HOLD_MS)
                {
                    return;
                }
            }
            else if (!nextIsStart(len))
            {
                /* the byte after this frame is not a frame start: bytes were
                 * lost inside it (or this is a false header met while
                 * hunting).  Start/continue a hunt; the skipped bytes are
                 * turned into silence frames when it ends. */
                holding = 0U;
                DBG_SKIP(avail);
                hw_uart_rx_consume(1);
                stats.badBytes++;
                if (!resync)
                {
                    resyncFrame = 1U;
                }
                NOTE_SKIP();
                continue;
            }
            else
            {
                if (resync)
                {
                    endResync();                    /* verified, in step  */
                }
            }
        }
        holding = 0U;
        for (i = 0; i < len; i++)
        {
            frame[i] = hw_uart_rx_peek(i);
        }
        hw_uart_rx_consume(len);
        DBG_GOOD(len, frame[2]);
        handleFrame(frame, len);
    }
}

/* ------------------------------------------------------------------ */
/* RX streaming                                                       */
/* ------------------------------------------------------------------ */
static void sendRxFrames(void)
{
    uint8_t f[RX_FRAME_BYTES];

    while ((uint32_t)(rxWrCount - rxRdCount) != 0U)
    {
        if (!configured)
        {
            rxRdCount++;                            /* nobody listening  */
            continue;
        }

        f[0] = OMT_START;
        f[1] = RX_FRAME_BYTES;
        f[2] = (uint8_t)(OMT_SAMP_DATA | 0U);
        memcpy(f + 3, rxBuf[rxRdCount & (RX_FRAMES - 1)], SAMPLES_PER_FRAME * 2);

        if (!hw_uart_tx(f, RX_FRAME_BYTES))
        {
            break;                                  /* UART busy, retry  */
        }
        rxRdCount++;
        stats.rxFramesToHost++;
        ledPulse(2, 20);
    }
}

/* [0x61][0x0B][0xB0][offset int32 BE][k uint32 BE]
 *   k      index of a TX sample (0 = first sample of the downlink)
 *   offset the RX sample being captured as TX sample k went on air,
 *          relative to the next RX sample the host will receive. */
static void sendTimingReport(void)
{
    uint8_t  f[11];
    uint32_t s1;
    uint32_t s2;
    uint32_t k;
    uint32_t r;
    int32_t  off;

    if (!tmReady || !configured)
    {
        return;
    }
    s1 = tmSeq;
    k = tmK;
    r = tmR;
    s2 = tmSeq;
    if ((s1 & 1U) || s1 != s2)
    {
        return;                                     /* being rewritten   */
    }
    off = (int32_t)(r - rxRdCount * SAMPLES_PER_FRAME);

    f[0] = OMT_START;
    f[1] = 11;
    f[2] = OMT_TX_TIMING;
    f[3] = (uint8_t)((uint32_t)off >> 24);
    f[4] = (uint8_t)((uint32_t)off >> 16);
    f[5] = (uint8_t)((uint32_t)off >> 8);
    f[6] = (uint8_t)off;
    f[7] = (uint8_t)(k >> 24);
    f[8] = (uint8_t)(k >> 16);
    f[9] = (uint8_t)(k >> 8);
    f[10] = (uint8_t)k;
    if (hw_uart_tx(f, 11))
    {
        tmReady = 0;
    }
}

/* ------------------------------------------------------------------ */
/* LEDs and housekeeping                                              */
/* ------------------------------------------------------------------ */
static void updateLeds(void)
{
    uint32_t now = hw_millis();
    uint8_t  i;
    bool     on[NUM_LEDS];

    for (i = 0; i < NUM_LEDS; i++)
    {
        on[i] = ((int32_t)(ledUntil[i] - now) > 0);     /* pulse LEDs */
    }

    if (!configured || (uint32_t)(now - lastHostMs) > 5000U)
    {
        on[0] = ((now / 500U) & 1U) != 0U;              /* waiting for host */
    }
    else
    {
        on[0] = (now % 1000U) < 80U;                    /* linked */
    }
    on[3] = (txState != TX_IDLE);                       /* PTT */
    on[4] = (txState == TX_PLAY);                       /* audio playing */
    on[7] = on[7] || hw_ext_clock_missing();

    for (i = 0; i < NUM_LEDS; i++)
    {
        if (ledPin[i] != PIN_NONE)
        {
            hw_led(i, on[i]);
        }
    }
}

#if DEBUG_FRAMES
/* tiny "label=number" formatter (no printf, keeps the image small) */
static uint8_t dbgText(uint8_t *f, uint8_t n, const char *label, uint32_t v)
{
    char digits[10];
    uint8_t d = 0;

    while (*label)
    {
        f[n++] = (uint8_t)*label++;
    }

    do
    {
        digits[d++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U);
    while (d)
    {
        f[n++] = (uint8_t)digits[--d];
    }
    return n;
}

static uint8_t dbgSigned(uint8_t *f, uint8_t n, const char *label, int32_t v)
{
    n = dbgText(f, n, label, 0);                    /* writes "label0" */
    n--;                                            /* drop the 0      */
    if (v < 0)
    {
        f[n++] = '-';
        v = -v;
    }
    return dbgText(f, n, "", (uint32_t)v);
}
#endif

static void housekeeping(void)
{
    uint32_t now = hw_millis();
    uint32_t v;

    if (isrActivity)
    {
        isrActivity = 0;
        ledPulse(5, 100);
    }
    if (isrClip)
    {
        isrClip = 0;
        ledPulse(6, 200);
    }

    v = isrUnderrunCount;
    if (v != lastUnderrunSeen)
    {
        stats.txUnderrun += v - lastUnderrunSeen;
        lastUnderrunSeen = v;
        ledPulse(7, 500);
    }
    v = isrClipCount;
    if (v != lastClipSeen)
    {
        stats.adcClip += v - lastClipSeen;
        lastClipSeen = v;
    }
    v = isrRxDropped;
    if (v != lastDropSeen)
    {
        stats.rxDropped += v - lastDropSeen;
        lastDropSeen = v;
        ledPulse(7, 500);
    }
    v = hw_uart_errors();
    if (v != lastUartErr)
    {
        lastUartErr = v;
        ledPulse(7, 500);
    }

    /* host died without sending EOT: do not stay keyed forever */
    if (txState != TX_IDLE && (uint32_t)(now - lastDataMs) > TX_UNDERRUN_TIMEOUT_MS)
    {
        keyDown();
    }

    /* self-heal: the engine is idle, so PTT must be off whatever happened
     * to the pin (same context as the key-up, so no race with it) */
    if (txState == TX_IDLE)
    {
        hw_ptt(false);
    }

#if DEBUG_FRAMES
    {
        static uint32_t lastDbg;

        if ((uint32_t)(now - lastDbg) >= 5000U && configured)
        {
            uint8_t f[240];
            uint8_t n = 3;
            uint32_t pk = dbgPeak;
            uint32_t clipNow = isrClipCount;
            uint32_t clipWin = clipNow - dbgClipSeen;   /* ADC rail hits this window */
            uint32_t lowWater;
            int32_t  dc = 0;
            uint32_t cnt = dbgCount;

            if (cnt)
            {
                dc = (int32_t)(((int64_t)dbgSum * 16) / (int32_t)cnt);
            }
            dbgClipSeen = clipNow;
            lowWater = dbgTxMin;
            dbgTxMin = 0xFFFFU;
            dbgPeak = 0;
            dbgSum = 0;
            dbgCount = 0;

            lastDbg = now;
            n = dbgText(f, n, "FW rx=", stats.rxFramesToHost);
            n = dbgText(f, n, " tx=", stats.txFramesFromHost);
            n = dbgText(f, n, " ur=", stats.txUnderrun);
            n = dbgText(f, n, " ov=", stats.txOverflow);
            n = dbgText(f, n, " bad=", stats.badBytes);
            n = dbgText(f, n, " ue=", hw_uart_errors());
            n = dbgText(f, n, " lf=", stats.lostFrames);
            n = dbgText(f, n, " ev=", dbgSkipEvents);
            n = dbgText(f, n, " pl=", dbgPrevLen);
            n = dbgText(f, n, " pt=", dbgPrevType);
            n = dbgText(f, n, " sk=", dbgSk[0]);
            n = dbgText(f, n, ",", dbgSk[1]);
            n = dbgText(f, n, ",", dbgSk[2]);
            n = dbgText(f, n, ",", dbgSk[3]);
            n = dbgText(f, n, ",", dbgSk[4]);
            n = dbgText(f, n, ",", dbgSk[5]);
            n = dbgText(f, n, " st=", txState);
            n = dbgText(f, n, " eot=", txEot);
            n = dbgText(f, n, " ld=", now - lastDataMs);
            n = dbgText(f, n, " pre=", dbgPre[0]);
            n = dbgText(f, n, ",", dbgPre[1]);
            n = dbgText(f, n, ",", dbgPre[2]);
            n = dbgText(f, n, ",", dbgPre[3]);
            n = dbgText(f, n, ",", dbgPre[4]);
            n = dbgText(f, n, ",", dbgPre[5]);
            n = dbgText(f, n, " rc=", dbgRecovered);
            n = dbgText(f, n, " nb=", dbgBadNext);
            n = dbgText(f, n, " rl=", dbgRunLast);
            n = dbgText(f, n, "/", dbgRunMax);
            n = dbgText(f, n, " mx=", dbgMaxAvail);
            n = dbgText(f, n, " gp=", dbgMaxGap);
            n = dbgText(f, n, " pk=", pk);
            n = dbgSigned(f, n, " dc=", dc);
            n = dbgText(f, n, " clip=", clipWin);
            n = dbgText(f, n, " ptt=", hw_ptt_state());
            n = dbgText(f, n, " lw=", lowWater);          /* min TX fill (samples), 65535 = no TX */
            if (clipWin != 0U || pk >= RX_HOT_PEAK)
            {
                /* the radio's audio is too loud for the modem: lower rxLevel */
                static const char hot[] = " !RX-TOO-HOT";
                uint8_t k;

                for (k = 0; hot[k] != '\0'; k++)
                {
                    f[n++] = (uint8_t)hot[k];
                }
            }
            f[0] = OMT_START;
            f[1] = n;
            f[2] = OMT_DEBUG;
            hw_uart_tx(f, n);
        }
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Public                                                             */
/* ------------------------------------------------------------------ */
void engine_init(void)
{
    uint8_t i;

    memset(&stats, 0, sizeof(stats));
    resync = 0;
    resyncBytes = 0;
    resyncFrame = 0;
    holding = 0;
    txHead = txTail = 0;
    txState = TX_IDLE;
    txEot = 0;
    rxIdx = 0;
    rxWrCount = rxRdCount = 0;
    rxDcQ12 = 0;
    rxGainQ8 = RX_GAIN_Q8;
    rxInvert = 0;
    duplex = 0;
    txInvert = false;
    configured = false;
    lastHostMs = lastDataMs = hw_millis();
    for (i = 0; i < NUM_LEDS; i++)
    {
        ledUntil[i] = 0;
    }
    hw_ptt(false);
}

void engine_poll(void)
{
    hw_uart_poll();
    parseUart();
    sendRxFrames();
#if TX_TIMING_REPORT
    sendTimingReport();                 /* after the RX frames: see offset */
#endif
    housekeeping();
    updateLeds();
    hw_uart_poll();
}

const engine_stats_t *engine_stats(void)
{
    return &stats;
}

bool engine_is_keyed(void)
{
    return txState != TX_IDLE;
}

bool engine_configured(void)
{
    return configured;
}
