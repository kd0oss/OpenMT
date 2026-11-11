#include "remote_settings.h"

#include <cmath>

namespace openmt
{

/* The keys the window saves (see MainWindow::saveSettings).  The remote control's own settings (remoteEnabled, remotePort,
   remoteToken) are not here on purpose: a client must not be able to read the token or turn the remote control off. */
static const SettingDef gSettings[] =
{
    {"host", 's', false}, {"modem", 'i', false}, {"mode", 'i', false},
    {"ran", 'i', false}, {"src", 'i', false}, {"dst", 'i', false}, {"group", 'b', false}, {"layout", 'i', false},
    {"dmrColorCode", 'i', false}, {"dmrSrc", 'i', false}, {"dmrDst", 'i', false}, {"dmrGroup", 'b', false},
    {"dmrRepeater", 'i', false}, {"dmrSlot", 'i', false}, {"dmrTiming", 'i', false},
    {"p25Nac", 's', false}, {"p25Src", 'i', false}, {"p25Dst", 'i', false}, {"p25Group", 'b', false},
    {"ysfCall", 's', false}, {"ysfDest", 's', false}, {"ysfDgid", 'i', false},
    {"dstarCall", 's', false}, {"dstarSuffix", 's', false}, {"dstarUr", 's', false}, {"dstarRpt1", 's', false},
    {"dstarRpt2", 's', false}, {"dstarMsg", 's', false},
    {"m17Call", 's', false}, {"m17Dest", 's', false}, {"m17Can", 'i', false}, {"m17Filter", 'i', false},
    {"m17Meta", 's', true}, {"m17MsgTo", 's', true},
    {"fmTxTone", 'd', false}, {"fmToneLevel", 'd', false}, {"fmDtmfLevel", 'd', false}, {"fmDtmfHang", 'i', false},
    {"fmSquelch", 'i', false}, {"fmRxTone", 'd', false}, {"fmDelay", 'i', false}, {"fmSquelchLevel", 'd', false},
    {"fmAuto", 'b', false},
    {"ambeDevice", 's', false}, {"ambeBaud", 's', false}, {"ambeChunk", 'i', false}, {"ambeGap", 'i', false},
    {"noVocoder", 'b', false}, {"capture", 'b', false}, {"invertRx", 'b', false},
    {"txLevel", 'i', false}, {"rxLevel", 'i', false}, {"spectrumPort", 'i', false}, {"spectrumAuto", 'b', false},
    {"audioIn", 's', false}, {"audioOut", 's', false}, {"rate", 'i', false},
    {"micGain", 'i', true}, {"spkGain", 'i', true}
};

const SettingDef* remoteSettingTable(size_t& count)
{
    count = sizeof(gSettings) / sizeof(gSettings[0]);
    return gSettings;
}

std::string remoteCheckSettings(const std::map<std::string, Json>& values, bool connected)
{
    size_t n;
    const SettingDef* table = remoteSettingTable(n);

    for (std::map<std::string, Json>::const_iterator it = values.begin(); it != values.end(); ++it)
    {
        const SettingDef* def = NULL;

        for (size_t i = 0U; i < n; i++)
            if (it->first == table[i].key)
                def = &table[i];
        if (def == NULL)
            return "unknown setting \"" + it->first + "\"";

        const Json& v = it->second;
        bool typeOk = false;

        switch (def->type)
        {
            case 'i': typeOk = v.isNumber() && std::floor(v.asNumber()) == v.asNumber(); break;
            case 'd': typeOk = v.isNumber(); break;
            case 'b': typeOk = v.isBool(); break;
            case 's': typeOk = v.isString() || v.isNumber(); break;
        }
        if (!typeOk)
            return "the setting \"" + it->first + "\" has the wrong type";
        if (def->type == 's' && v.isString() && v.asString().size() > 200U)
            return "the setting \"" + it->first + "\" is too long";
        if (connected && !def->live)
            return "\"" + it->first + "\" cannot change while the radio is connected: disconnect first";
    }
    return std::string();
}

}
