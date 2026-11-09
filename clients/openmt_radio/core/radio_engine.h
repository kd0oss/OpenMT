/* OpenMT Radio: the engine.  It owns the link to Modem_Host, the vocoder and the radio mode, and is the whole
 * program apart from the user interface and the sound card. */

#ifndef OPENMT_RADIO_ENGINE_H
#define OPENMT_RADIO_ENGINE_H

#include <cstdio>
#include <memory>
#include <string>

#include "modem_link.h"
#include "dmr_mode.h"
#include "p25_mode.h"
#include "ysf_mode.h"
#include "dstar_mode.h"
#include "m17_mode.h"
#include "fm_mode.h"
#include "nxdn_mode.h"
#include "radio_mode.h"
#include "rf_monitor.h"
#include "iq_monitor.h"
#include "rf_spectrum.h"
#include "vocoder.h"

namespace openmt
{

class AmbeVocoder;

enum ModeId
{
    MODE_NXDN = 0,
    MODE_DMR,
    MODE_P25,
    MODE_YSF,
    MODE_DSTAR,
    MODE_M17,
    MODE_FM
};

/* true when the mode has an engine in this version */
bool modeAvailable(ModeId mode);
const char* modeLabel(ModeId mode);

struct EngineSettings
{
    ModeId           mode        = MODE_NXDN;
    ModemLink::Config link;
    NxdnConfig       nxdn;
    DmrConfig        dmr;
    P25Config        p25;
    YsfConfig        ysf;
    DstarConfig      dstar;
    M17Config        m17;
    FmConfig         fm;
    std::string      ambeDevice  = "/dev/ttyUSB0";
    unsigned int     ambeBaud    = 460800;
    int              ambeChunk   = 64;          /* bytes per write to the chip, 0 = whole packet */
    int              ambeGapUs   = 500;         /* pause after each chunk */
    bool             noVocoder   = false;       /* run without a dongle (tests, link checks) */
    unsigned int     spectrumPort = 18050;      /* UDP port of the HackRF bridge's RF spectrum (on the Modem_Host machine), 0 = do not ask */
    std::string      rxCapture;                 /* file that gets every sample received from Modem_Host (16 bit, 24 kHz), "" = none */
};

class RadioEngine
{
public:
    RadioEngine();
    ~RadioEngine();

    RadioEvents events;         /* set before start() */

    bool start(const EngineSettings& settings, std::string& error);
    void stop();
    bool running() const;

    void ptt(bool down);

    /* Tells Modem_Host to let go of this mode, if it is holding it (after a crash or a lost message it can keep the mode and
       then deliver samples to no one else). Only the mode this radio runs is affected: other protocol hosts are untouched */
    bool releaseModem();
    bool dtmfSupported() const;
    bool sendDtmf(char digit);
    bool messagesSupported() const;
    bool sendMessage(const std::string& to, const std::string& text, std::string& error);
    void setMetaText(const std::string& text);
    void pushMic(const int16_t* pcm8k, size_t count);

    std::string vocoderDescription() const;

    /* The signal monitor: it sees every sample received from Modem_Host (before the receive gain of the mode) and can be asked
       for an analysis at any time, from any thread */
    RfMonitor& monitor() { return monitor_; }

    /* The RF spectrum from the HackRF bridge (no frames from any other modem).  It is only asked for while setSpectrumWanted(true) */
    SpectrumClient& spectrum() { return spectrum_; }

    /* The signal monitor made from the IQ of the bridge (the HackRF and SX1255 bridges): the true deviation and frequency error.
       The IQ is only asked for while the Monitor tab shows it (spectrum().setIqWanted(true)) */
    IqMonitor& iq() { return iq_; }

    /* The AMBE3000R dongle.  Opening its serial port can reset it (DTR) and it needs seconds to come back, so the port is opened
       once and stays open until closeDongle() or the end of the program, whether the mode that runs uses the dongle or not.
       start() uses the open dongle for the modes that need it (and opens it itself if it is not open); a different device or baud
       rate than the open one closes and reopens it. */
    bool openDongle(const std::string& device, unsigned int baud, int chunkBytes, int gapUs, std::string& error);
    void closeDongle();
    bool dongleOpen() const;

    /* From the link's reader thread, also called by the tests */
    void onLinkClosed();
    std::function<void()> linkClosed;

private:
    ModemLink                    link_;
    Vocoder*                     vocoder_;          /* the dongle, or nullVoc_: never owned here */
    std::unique_ptr<Vocoder>     nullVoc_;          /* the stand in of a run that does not use the dongle */
    std::unique_ptr<AmbeVocoder> dongle_;           /* kept open between runs */
    std::string                  dongleDevice_;
    unsigned int                 dongleBaud_;
    std::unique_ptr<RadioMode>   mode_;
    RfMonitor                    monitor_;
    IqMonitor                    iq_;               /* before spectrum_: the client thread feeds it, so it is destroyed after */
    SpectrumClient               spectrum_;
    bool                         running_;
    FILE*                        capture_;
};

}

#endif
