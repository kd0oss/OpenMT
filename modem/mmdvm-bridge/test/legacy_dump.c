/* Prints the frames the old host code (mmdvm.c) builds, for comparison.
 * usage: legacy_dump hs|full dstar,dmr,...  */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "mmdvm.h"
#include "RingBuffer.h"

RingBuffer modemCommandBuffer;
char modem[20] = "mmdvmhs";
bool modem_trace, modem_debug, modem_duplex, modem_rxInvert, modem_txInvert, modem_pttInvert, modem_useCOSAsLockout;
unsigned int modem_txDelay = 100U;
char modem_rxFrequency[11], modem_txFrequency[11];
uint8_t modem_rxLevel = 100U, modem_rfLevel;
int modem_rxDCOffset, modem_txDCOffset;
extern bool modem_dstarEnabled, modem_dmrEnabled, modem_ysfEnabled, modem_p25Enabled, modem_nxdnEnabled, modem_fmEnabled, modem_m17Enabled;

static void dump(void)
{
    uint8_t buf[64];
    unsigned n = RingBuffer_dataSize(&modemCommandBuffer);
    RingBuffer_getData(&modemCommandBuffer, buf, n);
    for (unsigned i = 0; i < n; i++) printf("%02x", buf[i]);
    printf("\n");
}

int main(int argc, char** argv)
{
    RingBuffer_Init(&modemCommandBuffer, 300);
    bool hs = strcmp(argv[1], "hs") == 0;
    snprintf(modem, sizeof(modem), "%s", hs ? "mmdvmhs" : "mmdvm");
    modem_dstarEnabled = strstr(argv[2], "dstar") != NULL;
    modem_dmrEnabled   = strstr(argv[2], "dmr") != NULL;
    modem_ysfEnabled   = strstr(argv[2], "ysf") != NULL;
    modem_p25Enabled   = strstr(argv[2], "p25") != NULL;
    modem_nxdnEnabled  = strstr(argv[2], "nxdn") != NULL;
    modem_fmEnabled    = strstr(argv[2], "fm") != NULL;
    modem_m17Enabled   = strstr(argv[2], "m17") != NULL;
    if (hs) set_ConfigHS("modem1"); else set_Config("modem1");
    dump();
    if (hs)
    {
        setFrequency("449900000", "444900000", "444900000", 255);
        dump();
    }
    return 0;
}
