/* OpenMT Radio: what a radio mode (NXDN, DMR, P25, YSF, D-STAR, M17, FM) looks like to the engine.
 *
 * A mode turns microphone speech into the samples Modem_Host sends to the modem, and the samples the modem
 * demodulated back into speech and call information.  The engine owns the link and the vocoder. */

#ifndef OPENMT_RADIO_MODE_H
#define OPENMT_RADIO_MODE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace openmt
{

enum CallEvent
{
    CALL_RX_START = 1,      /* a received call began (the ids may still be unknown: 0) */
    CALL_RX_INFO,           /* the ids of the received call became known */
    CALL_RX_END,            /* normal end */
    CALL_RX_LOST,           /* the signal went away without an end */
    CALL_TX_START,
    CALL_TX_END
};

struct CallInfo
{
    CallEvent    event;
    unsigned int src;       /* source id, 0 = unknown */
    unsigned int dst;
    bool         group;
    float        ber;       /* percent, received calls */
    std::string  text;      /* mode specific: callsigns, text ... */
};

/* A text message: sent by this radio (received false) or heard (received true) */
struct MessageInfo
{
    bool         received;
    std::string  from;
    std::string  to;
    std::string  text;      /* UTF-8 */
};

struct RadioEvents
{
    /* 8 kHz speech of the received call, 160 samples at a time */
    std::function<void(const int16_t*, size_t)> rxAudio;
    std::function<void(const CallInfo&)>        call;
    std::function<void(const std::string&)>     log;
    std::function<void(const MessageInfo&)>     message;
};

class RadioMode
{
public:
    virtual ~RadioMode() {}

    /* The label and modem type the link registers with */
    virtual std::string modeName() const = 0;
    virtual std::string modemType() const = 0;

    virtual void start() = 0;
    virtual void stop() = 0;

    /* From the link's reader thread: demodulated samples, 24 kHz */
    virtual void rxSamples(const int16_t* samples, size_t count) = 0;

    virtual void pttDown() = 0;
    virtual void pttUp() = 0;
    /* 8 kHz speech from the microphone; ignored while not transmitting */
    virtual void micAudio(const int16_t* pcm, size_t count) = 0;

    virtual bool transmitting() const = 0;

    /* DTMF: only a mode whose host listens for it (FM) accepts digits.  sendDtmf() keys the transmitter by itself when
       the key is not down and holds it a moment for the next digit; 0-9 * # A-D */
    virtual bool supportsDtmf() const { return false; }
    virtual bool sendDtmf(char digit) { (void)digit; return false; }

    /* Text: a mode that can send a text message (M17 packet mode) says so.  sendMessage() queues it and returns at once; false
       and an error text when it cannot be sent.  setMetaText() sets the text that goes with every voice transmission
       (M17 LSF metadata) from the next one on */
    virtual bool supportsMessages() const { return false; }
    virtual bool sendMessage(const std::string& to, const std::string& text, std::string& error)
    {
        (void)to; (void)text;
        error = "this mode cannot send messages";
        return false;
    }
    virtual void setMetaText(const std::string& text) { (void)text; }

    /* From the link's reader thread: Modem_Host says whether the modem is duplex */
    virtual void modemDuplex(bool duplex) { (void)duplex; }
    /* From the link's reader thread: the modem reports that TX sample 'k' of this transmission is on the air as the RX
       sample 'offset' samples after the next one to be delivered (an OPENMT_TX_TIMING report, "TXTM") */
    virtual void txTiming(int32_t offset, uint32_t k) { (void)offset; (void)k; }
};

}

#endif
