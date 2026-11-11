/* OpenMT Radio: the settings a remote client can read and change.  They are the keys the window saves (see
 * MainWindow::saveSettings), with their type and whether they may change while the radio runs.  The remote control's own
 * settings (remoteEnabled, remotePort, remoteToken) are not in the table on purpose: a client must not be able to read the
 * token or turn the remote control off. */

#ifndef OPENMT_REMOTE_SETTINGS_H
#define OPENMT_REMOTE_SETTINGS_H

#include <cstddef>
#include <map>
#include <string>

#include "remote_json.h"

namespace openmt
{

struct SettingDef
{
    const char* key;
    char        type;           /* 'i' integer, 'd' number, 'b' boolean, 's' text */
    bool        live;           /* may change while the radio runs */
};

const SettingDef* remoteSettingTable(size_t& count);

/* "" when the values are all known settings of the right type (and, while the radio is connected, live ones), else why not */
std::string remoteCheckSettings(const std::map<std::string, Json>& values, bool connected);

}

#endif
