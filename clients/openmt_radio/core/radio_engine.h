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
#include "vocoder.h"

namespace openmt
{

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
    void pushMic(const int16_t* pcm8k, size_t count);

    std::string vocoderDescription() const;

    /* From the link's reader thread, also called by the tests */
    void onLinkClosed();
    std::function<void()> linkClosed;

private:
    ModemLink                    link_;
    std::unique_ptr<Vocoder>     vocoder_;
    std::unique_ptr<RadioMode>   mode_;
    bool                         running_;
    FILE*                        capture_;
};

}

#endif
