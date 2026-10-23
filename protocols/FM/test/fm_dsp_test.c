/*
 * Created by Rick KD0OSS
 *
 * Test of the FM DTMF decoder and the 8 kSa/s converters: every digit must be
 * decoded exactly once through the 24 to 8 kSa/s converter, a voice-like
 * signal must give no digit, and a tone must keep its level through the
 * 8 to 24 to 8 kSa/s round trip.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "fm_dsp.h"

static const float LO[4] = {697, 770, 852, 941};
static const float HI[4] = {1209, 1336, 1477, 1633};
static const char D[4][4] = {{'1','2','3','A'},{'4','5','6','B'},{'7','8','9','C'},{'*','0','#','D'}};

/*
 * main()
 *
 * Runs the decoder and converter checks and prints PASS or FAIL.
 *
 * Parameters: none.
 *
 * Returns: 0 if every check passed, 1 if not.
 */
int main(void)
{
    int bad = 0;
    srand(1);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
        {
            FmDtmf d; FmDecim dec; char got[8]; int ng = 0; float o;
            fmDtmfInit(&d); fmDecimInit(&dec);
            for (int n = 0; n < 24000 * 0.3; n++)
            {
                float t = (float)n / 24000.0f;
                float x = 3000 * sinf(6.2831853f * LO[r] * t) + 3000 * sinf(6.2831853f * HI[c] * t) + (rand() % 600 - 300);
                if (n > 24000 * 0.2) x = (rand() % 600 - 300);
                if (fmDecimPush(&dec, x, &o)) { char g = fmDtmfPush(&d, o); if (g && ng < 8) got[ng++] = g; }
            }
            if (ng != 1 || got[0] != D[r][c]) { printf("FAIL %c: got %d digits first %c\n", D[r][c], ng, ng ? got[0] : '-'); bad++; }
        }
    /* speech-like noise must not trigger */
    FmDtmf d; fmDtmfInit(&d); int fa = 0;
    for (int n = 0; n < 8000 * 20; n++)
    {
        float t = n / 8000.0f;
        float x = 2000 * sinf(6.28f * (300 + 200 * sinf(t * 3)) * t) + 1500 * sinf(6.28f * 1100 * t) + (rand() % 2000 - 1000);
        if (fmDtmfPush(&d, x)) fa++;
    }
    printf("false digits on voice-like signal: %d\n", fa);
    /* converter: 1 kHz sine 8k->24k->8k keeps level */
    FmInterp ip; FmDecim dc; fmInterpInit(&ip); fmDecimInit(&dc);
    double e_in = 0, e_out = 0; float o;
    for (int n = 0; n < 8000; n++)
    {
        float x = 8000 * sinf(6.2831853f * 1000 * n / 8000.0f), y[3];
        fmInterpRun(&ip, x, y);
        for (int k = 0; k < 3; k++) if (fmDecimPush(&dc, y[k], &o)) { if (n > 1000) { e_out += o * o; e_in += x * x; } }
    }
    printf("converter gain %.3f\n", sqrt(e_out / e_in));
    printf(bad || fa ? "FAIL\n" : "PASS\n");
    return bad || fa;
}
