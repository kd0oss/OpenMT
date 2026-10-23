/* Created by Rick KD0OSS */
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ysf_wiresx.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t DX_REQ[]    = {0x5DU, 0x71U, 0x5FU};    /* followed by 2B (FT-70D) 29 (FTM-100D) 28 (FT2D) */
static const uint8_t CONN_REQ[]  = {0x5DU, 0x23U, 0x5FU};
static const uint8_t DISC_REQ[]  = {0x5DU, 0x2AU, 0x5FU};
static const uint8_t ALL_REQ[]   = {0x5DU, 0x66U, 0x5FU};
static const uint8_t CAT_REQ[]   = {0x5DU, 0x67U, 0x5FU};

static const uint8_t DX_RESP[]   = {0x5DU, 0x51U, 0x5FU, 0x26U};
static const uint8_t CONN_RESP[] = {0x5DU, 0x41U, 0x5FU, 0x26U};
static const uint8_t DISC_RESP[] = {0x5DU, 0x41U, 0x5FU, 0x26U};
static const uint8_t ALL_RESP[]  = {0x5DU, 0x46U, 0x5FU, 0x26U};

static const uint8_t DEFAULT_FICH[] = {0x20U, 0x00U, 0x01U, 0x00U};

/* "YSFD" + 20 spaces + "ALL" + 6 spaces, and the 0 byte that ends the C string (34 bytes) */
static const char NET_HEADER[] = "YSFD                    ALL      ";

#define TIMER_MS        1000U       /* a reply is made this long after the request */
#define BUSY_MS         3000U
#define TX_INTERVAL_MS  90U

/*
 * addCRC()
 *
 * Makes the check sum of Wires-X data: the sum of the bytes, modulo 256.
 *
 * Parameters:
 *   in     - The bytes.
 *   length - Number of bytes.
 *
 * Returns: The check sum.
 */
static uint8_t addCRC(const uint8_t* in, unsigned int length)
{
    uint8_t crc = 0U;

    for (unsigned int i = 0U; i < length; i++)
        crc += in[i];

    return crc;
}

/*
 * padString()
 *
 * Copies a string into a field of 'width' characters padded with spaces.
 *
 * Parameters:
 *   out   - The field; width + 1 bytes, 0 terminated.
 *   width - Number of characters in the field.
 *   text  - The text; cut off when longer than width.
 *
 * Returns: nothing.
 */
static void padString(char* out, unsigned int width, const char* text)
{
    size_t len = strlen(text);

    if (len > width)
        len = width;

    memset(out, ' ', width);
    memcpy(out, text, len);
    out[width] = '\0';
}

/*
 * wxInit()
 *
 * Prepares a Wires-X handler.  The node name is the callsign with the suffix
 * after a dash.
 *
 * Parameters:
 *   w          - The handler.
 *   callsign   - The station callsign.
 *   suffix     - The node suffix, or NULL or empty for none.
 *   reflectors - The reflector list that the replies are made from.
 *   send       - Called for every frame of a reply, when it is time to
 *                transmit it.
 *   ctx        - Passed to send.
 *
 * Returns: nothing.
 */
void wxInit(WiresX* w, const char* callsign, const char* suffix, RefList* reflectors, WxSendFn send, void* ctx)
{
    char node[64];

    memset(w, 0x00, sizeof(*w));

    snprintf(node, sizeof(node), "%s", callsign);

    if (suffix != NULL && suffix[0] != '\0')
    {
        strncat(node, "-", sizeof(node) - strlen(node) - 1U);
        strncat(node, suffix, sizeof(node) - strlen(node) - 1U);
    }

    padString(w->node, 10U, node);
    padString(w->callsign, 10U, callsign);
    memcpy(w->name, "              ", 15U);
    w->reflectors = reflectors;
    w->send       = send;
    w->ctx        = ctx;
}

/*
 * wxSetInfo()
 *
 * Sets the name and the frequencies of the node and works out what depends
 * on them: the five digit node id (a hash of the name), the callsign data of
 * the frames and the header of the network packets.
 *
 * Parameters:
 *   w           - The handler.
 *   name        - The name of the node.
 *   txFrequency - Transmit frequency in Hz.
 *   rxFrequency - Receive frequency in Hz.
 *
 * Returns: nothing.
 */
void wxSetInfo(WiresX* w, const char* name, uint32_t txFrequency, uint32_t rxFrequency)
{
    uint32_t hash = 0U;
    char id[16];

    w->txFrequency = txFrequency;
    w->rxFrequency = rxFrequency;

    padString(w->name, 14U, name);

    for (size_t i = 0U; name[i] != '\0'; i++)
    {
        hash += (uint32_t)(int32_t)(signed char)name[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }

    /* Final avalanche */
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    snprintf(id, sizeof(id), "%05u", hash % 100000U);
    memcpy(w->id, id, 6U);

    memset(w->csd1, '*', 20U);
    memset(w->csd2, ' ', 20U);
    memset(w->csd3, ' ', 20U);

    memcpy(w->csd1 + 10U, w->node, 10U);
    memcpy(w->csd2, w->callsign, 10U);
    memcpy(w->csd3, w->id, 5U);
    memcpy(w->csd3 + 15U, w->id, 5U);

    memcpy(w->header, NET_HEADER, 34U);
    memcpy(w->header + 4U, w->callsign, 10U);
    memcpy(w->header + 14U, w->node, 10U);
}

/*
 * wxIsBusy()
 *
 * Tells if Wires-X is busy: a connect was asked for or answered less than 3
 * seconds ago.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: true if busy.
 */
bool wxIsBusy(const WiresX* w)
{
    return w->busy;
}

/*
 * wxGetReflector()
 *
 * Gets the reflector that Wires-X reports as linked.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: The reflector that is linked, or NULL if there is none.
 */
const Reflector* wxGetReflector(const WiresX* w)
{
    return w->reflector;
}

/*
 * wxSetReflector()
 *
 * Sets the reflector that is reported as linked.
 *
 * Parameters:
 *   w         - The handler.
 *   reflector - The reflector, or NULL for none.
 *
 * Returns: nothing.
 */
void wxSetReflector(WiresX* w, const Reflector* reflector)
{
    w->reflector = reflector;
}

/*
 * startTimer()
 *
 * Starts the timer that delays a reply after the request.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void startTimer(WiresX* w)
{
    w->timerMs      = 0U;
    w->timerRunning = true;
}

/*
 * processDX()
 *
 * Handles the DX request: the node information is sent after the timer.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void processDX(WiresX* w)
{
    w->status = WXSI_DX;
    startTimer(w);
}

/*
 * processCategory()
 *
 * Handles the category request: collects the reflectors of the ids in it (up
 * to 20), then the category list is sent after the timer.
 *
 * Parameters:
 *   w    - The handler.
 *   data - The request after the command: two digits for the number of ids,
 *          then 5 digit ids.
 *
 * Returns: nothing.
 */
static void processCategory(WiresX* w, const uint8_t* data)
{
    char buffer[8];
    unsigned int len;

    memcpy(buffer, data + 5U, 2U);
    buffer[2] = '\0';

    len = (unsigned int)atoi(buffer);

    if (len == 0U || len > 20U)
        return;

    w->categoryCount = 0U;

    for (unsigned int j = 0U; j < len; j++)
    {
        char id[6];
        const Reflector* refl;

        memcpy(id, data + 7U + j * 5U, 5U);
        id[5] = '\0';

        refl = reflistFindById(w->reflectors, id);

        if (refl != NULL)
            w->category[w->categoryCount++] = refl;
    }

    w->status = WXSI_CATEGORY;
    startTimer(w);
}

/*
 * processAll()
 *
 * Handles the request for the list of all reflectors (the list is sent from
 * the number given) or for a search (the text is kept), then the answer is
 * sent after the timer.
 *
 * Parameters:
 *   w    - The handler.
 *   data - The request after the command.
 *
 * Returns: nothing.
 */
static void processAll(WiresX* w, const uint8_t* data)
{
    char buffer[4];

    memcpy(buffer, data + 2U, 3U);
    buffer[3] = '\0';

    if (data[0] == '0' && data[1] == '1')
    {
        w->start = (unsigned int)atoi(buffer);

        if (w->start > 0U)
            w->start--;

        w->status = WXSI_ALL;
        startTimer(w);
    }
    else if (data[0] == '1' && data[1] == '1')
    {
        w->start = (unsigned int)atoi(buffer);

        if (w->start > 0U)
            w->start--;

        memcpy(w->search, data + 5U, 16U);
        w->search[16] = '\0';

        w->status = WXSI_SEARCH;
        startTimer(w);
    }
}

/* ------------------------------------------------------------------------ */
/* Replies                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * calculateFT()
 *
 * Works out the frame total field for the rest of a reply.
 *
 * Parameters:
 *   length - Length of the reply.
 *   offset - How much of it was sent already.
 *
 * Returns: The frame total, 1 to 7.
 */
static uint8_t calculateFT(unsigned int length, unsigned int offset)
{
    length -= offset;

    if (length > 220U) return 7U;
    if (length > 180U) return 6U;
    if (length > 140U) return 5U;
    if (length > 100U) return 4U;
    if (length > 60U)  return 3U;
    if (length > 20U)  return 2U;

    return 1U;
}

/*
 * emit()
 *
 * Queues a frame of a reply for transmission, with the network header and
 * the frame counter.  The frame is dropped if the queue is full.
 *
 * Parameters:
 *   w       - The handler.
 *   frame   - The frame, YSF_FRAME_LENGTH_BYTES bytes.
 *   counter - The frame counter; an odd value marks the last frame.
 *
 * Returns: nothing.
 */
static void emit(WiresX* w, const uint8_t* frame, uint8_t counter)
{
    unsigned int next = (w->qHead + 1U) % WX_TX_QUEUE;
    uint8_t* e;

    if (next == w->qTail)
        return;                 /* full: the frame is dropped, as the ring buffer of the original does */

    e = w->queue[w->qHead];
    e[0] = counter & 0x01U;
    memcpy(e + 1U, w->header + 4U, 30U);
    e[1U + 30U] = counter;
    memcpy(e + 1U + 31U, frame, YSF_FRAME_LENGTH_BYTES);

    w->qHead = next;
}

/*
 * createReply()
 *
 * Makes the frames of a reply from its data: a header frame, data FR frames
 * with the data in the callsign fields, and a terminator, and queues them.
 *
 * Parameters:
 *   w      - The handler.
 *   in     - The data of the reply.
 *   length - Number of bytes in in.
 *
 * Returns: nothing.
 */
static void createReply(WiresX* w, const uint8_t* in, unsigned int length)
{
    uint8_t data[1300];
    uint8_t frame[YSF_FRAME_LENGTH_BYTES];
    YsfFich fich;
    uint8_t bt = 0U;
    uint8_t ft;
    uint8_t seqNo = 0U;
    uint8_t fn = 0U;
    uint8_t bn = 0U;
    unsigned int offset = 0U;

    /* the callers' buffers are zero beyond their data, and the frames below read up to a whole block past it */
    memset(data, 0x00, sizeof(data));
    memcpy(data, in, length < 1100U ? length : 1100U);

    if (length > 260U)
    {
        bt = 1U;
        bt += (uint8_t)((length - 260U) / 259U);

        length += bt;
    }

    if (length > 20U)
    {
        unsigned int blocks = (length - 20U) / 40U;

        if ((length % 40U) > 0U)
            blocks++;

        length = blocks * 40U + 20U;
    }
    else
    {
        length = 20U;
    }

    ft = calculateFT(length, 0U);

    /* The header */
    memset(frame, 0x00, sizeof(frame));
    ysfAddSync(frame);

    ysfFichInit(&fich);
    memcpy(fich.b, DEFAULT_FICH, 4U);
    ysfFichSetFI(&fich, YSF_FI_HEADER);
    ysfFichSetBT(&fich, bt);
    ysfFichSetFT(&fich, ft);
    ysfFichEncode(&fich, frame);

    ysfPayloadWriteCsd(frame, 0U, w->csd1);
    ysfPayloadWriteCsd(frame, 1U, w->csd2);

    emit(w, frame, seqNo);
    seqNo += 2U;

    ysfFichSetFI(&fich, YSF_FI_COMMUNICATIONS);

    while (offset < length)
    {
        switch (fn)
        {
            case 0U:
                ft = calculateFT(length, offset);
                ysfPayloadWriteCsd(frame, 0U, w->csd1);
                ysfPayloadWriteCsd(frame, 1U, w->csd2);
                break;

            case 1U:
                ysfPayloadWriteCsd(frame, 0U, w->csd3);

                if (bn == 0U)
                {
                    ysfPayloadWriteCsd(frame, 1U, data + offset);
                    offset += 20U;
                }
                else
                {
                    /* All subsequent entries start with 0x00U */
                    uint8_t temp[20];

                    memcpy(temp + 1U, data + offset, 19U);
                    temp[0] = 0x00U;
                    ysfPayloadWriteCsd(frame, 1U, temp);
                    offset += 19U;
                }
                break;

            default:
                ysfPayloadWriteCsd(frame, 0U, data + offset);
                offset += 20U;
                ysfPayloadWriteCsd(frame, 1U, data + offset);
                offset += 20U;
                break;
        }

        ysfFichSetFT(&fich, ft);
        ysfFichSetFN(&fich, fn);
        ysfFichSetBT(&fich, bt);
        ysfFichSetBN(&fich, bn);
        ysfFichEncode(&fich, frame);

        emit(w, frame, seqNo);
        seqNo += 2U;

        fn++;

        if (fn >= 8U)
        {
            fn = 0U;
            bn++;
        }
    }

    /* The trailer */
    ysfFichSetFI(&fich, YSF_FI_TERMINATOR);
    ysfFichSetFN(&fich, fn);
    ysfFichSetBN(&fich, bn);
    ysfFichEncode(&fich, frame);

    ysfPayloadWriteCsd(frame, 0U, w->csd1);
    ysfPayloadWriteCsd(frame, 1U, w->csd2);

    emit(w, frame, (uint8_t)(seqNo | 0x01U));
}

/*
 * putFrequency()
 *
 * Writes the frequency fields of the DX reply: the transmit frequency and
 * the offset to the receive frequency with its sign.
 *
 * Parameters:
 *   w   - The handler.
 *   out - Receives 23 characters.
 *
 * Returns: nothing.
 */
static void putFrequency(const WiresX* w, uint8_t* out)
{
    unsigned int offset;
    char sign;
    unsigned int freqHz, freqkHz;
    char freq[40];

    if (w->txFrequency >= w->rxFrequency)
    {
        offset = w->txFrequency - w->rxFrequency;
        sign = '-';
    }
    else
    {
        offset = w->rxFrequency - w->txFrequency;
        sign = '+';
    }

    freqHz  = w->txFrequency % 1000000U;
    freqkHz = (freqHz + 500U) / 1000U;

    snprintf(freq, sizeof(freq), "%05u.%03u000%c%03u.%06u", w->txFrequency / 1000000U, freqkHz, sign,
             offset / 1000000U, offset % 1000000U);

    memcpy(out, freq, 23U);
}

/*
 * putStation()
 *
 * Writes the fields that the DX, connect and disconnect replies share: the
 * sequence number, the response type, the node id, the node name and the
 * node description.
 *
 * Parameters:
 *   w    - The handler.
 *   data - The reply.
 *   resp - The 4 byte response type.
 *
 * Returns: nothing.
 */
static void putStation(WiresX* w, uint8_t* data, const uint8_t* resp)
{
    data[0] = w->seqNo;
    memcpy(data + 1U, resp, 4U);
    memcpy(data + 5U, w->id, 5U);
    memcpy(data + 10U, w->node, 10U);
    memcpy(data + 20U, w->name, 14U);
}

/*
 * putReflector()
 *
 * Writes the fields of a reflector into a reply: id, name, user count and
 * description.
 *
 * Parameters:
 *   data - The reply.
 *   r    - The reflector.
 *
 * Returns: nothing.
 */
static void putReflector(uint8_t* data, const Reflector* r)
{
    memcpy(data + 36U, r->id, 5U);

    for (unsigned int i = 0U; i < 16U; i++)
        data[41U + i] = (uint8_t)toupper((unsigned char)r->name[i]);

    memcpy(data + 57U, r->count, 3U);
    memcpy(data + 70U, r->desc, 14U);
}

/*
 * sendDXReply()
 *
 * Makes the DX reply: the node information with the reflector that is
 * linked, or none.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendDXReply(WiresX* w)
{
    uint8_t data[150];

    memset(data, 0x00, sizeof(data));
    memset(data, ' ', 128U);

    putStation(w, data, DX_RESP);

    if (w->reflector == NULL)
    {
        data[34] = '1';     /* 0,1,2 seem valid */
        data[35] = '2';     /* 0 = offline 1 = busy 2 = disconnect 3 = normal */

        data[57] = '0';
        data[58] = '0';
        data[59] = '0';
    }
    else
    {
        data[34] = '1';
        data[35] = '3';

        putReflector(data, w->reflector);
    }

    putFrequency(w, data + 84U);

    data[127] = 0x03U;          /* end of data marker */
    data[128] = addCRC(data, 128U);

    createReply(w, data, 129U);

    w->seqNo++;
}

/*
 * sendConnectReply()
 *
 * Makes the reply to a connect request, with the reflector.  Nothing is sent
 * when there is no reflector.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendConnectReply(WiresX* w)
{
    uint8_t data[110];

    if (w->reflector == NULL)
        return;

    memset(data, 0x00, sizeof(data));
    memset(data, ' ', 90U);

    putStation(w, data, CONN_RESP);

    data[34] = '1';
    data[35] = '3';

    putReflector(data, w->reflector);

    data[84] = '0';
    data[85] = '0';
    data[86] = '0';
    data[87] = '0';
    data[88] = '0';

    data[89] = 0x03U;
    data[90] = addCRC(data, 90U);

    createReply(w, data, 91U);

    w->seqNo++;
}

/*
 * sendOfflineReply()
 *
 * Makes a reply for a node without a link (connect failed or disconnected).
 *
 * Parameters:
 *   w     - The handler.
 *   resp  - The 4 byte response type.
 *   state - The state character: '0' offline, '2' disconnected.
 *
 * Returns: nothing.
 */
static void sendOfflineReply(WiresX* w, const uint8_t* resp, char state)
{
    uint8_t data[110];

    memset(data, 0x00, sizeof(data));
    memset(data, ' ', 90U);

    putStation(w, data, resp);

    data[34] = '1';
    data[35] = (uint8_t)state;

    data[57] = '0';
    data[58] = '0';
    data[59] = '0';

    data[89] = 0x03U;
    data[90] = addCRC(data, 90U);

    createReply(w, data, 91U);

    w->seqNo++;
}

/*
 * sendConnectFailedReply()
 *
 * Makes the reply that tells the radio that the connect failed.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendConnectFailedReply(WiresX* w)
{
    sendOfflineReply(w, CONN_RESP, '0');
}

/*
 * sendDisconnectReply()
 *
 * Makes the reply that tells the radio that the node is disconnected.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendDisconnectReply(WiresX* w)
{
    sendOfflineReply(w, DISC_RESP, '2');
}

/*
 * putEntry()
 *
 * Writes one 50 byte list entry: kind, id, name, user count and description.
 *
 * Parameters:
 *   out  - Receives the 50 bytes.
 *   kind - '5' for the list of all reflectors or '1' for a search result.
 *   r    - The reflector.
 *
 * Returns: nothing.
 */
static void putEntry(uint8_t* out, char kind, const Reflector* r)
{
    memset(out, ' ', 50U);

    out[0] = (uint8_t)kind;
    memcpy(out + 1U, r->id, 5U);

    for (unsigned int i = 0U; i < 16U; i++)
        out[6U + i] = (uint8_t)toupper((unsigned char)r->name[i]);

    memcpy(out + 22U, r->count, 3U);
    memcpy(out + 35U, r->desc, 14U);

    out[49] = 0x0DU;
}

/*
 * finishList()
 *
 * Finishes a list reply: pads it to byte 1029 (with a carriage return at the
 * end of each 50 byte entry), adds the end marker and the check sum, and
 * makes the reply.
 *
 * Parameters:
 *   w      - The handler.
 *   data   - The reply.
 *   offset - Number of bytes made so far.
 *
 * Returns: nothing.
 */
static void finishList(WiresX* w, uint8_t* data, unsigned int offset)
{
    unsigned int k = 1029U - offset;

    for (unsigned int i = 0U; i < k; i++)
    {
        if ((i % 50U) == 49U && i > 0U)
            data[i + offset] = 0x0DU;
        else
            data[i + offset] = 0x20U;
    }

    offset += k;

    data[offset + 0U] = 0x03U;
    data[offset + 1U] = addCRC(data, offset + 1U);

    createReply(w, data, offset + 2U);

    w->seqNo++;
}

/*
 * sendAllReply()
 *
 * Makes the reply with a page of 20 of the list of all reflectors, from the
 * number that was asked for.  The list is read again when it is the first
 * page.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendAllReply(WiresX* w)
{
    uint8_t data[1100];
    char text[16];
    unsigned int total, n, offset;

    if (w->start == 0U)
        reflistRefresh(w->reflectors);

    memset(data, 0x00, sizeof(data));

    data[0] = w->seqNo;
    memcpy(data + 1U, ALL_RESP, 4U);

    data[5] = '2';
    data[6] = '1';

    memcpy(data + 7U, w->id, 5U);
    memcpy(data + 12U, w->node, 10U);

    total = w->reflectors->count;

    if (total > 999U)
        total = 999U;

    n = w->reflectors->count > w->start ? w->reflectors->count - w->start : 0U;

    if (n > 20U)
        n = 20U;

    snprintf(text, sizeof(text), "%03u%03u", 20U, total);
    memcpy(data + 22U, text, 6U);

    data[28] = 0x0DU;

    offset = 29U;

    for (unsigned int j = 0U; j < n; j++, offset += 50U)
        putEntry(data + offset, '5', &w->reflectors->items[j + w->start]);

    finishList(w, data, offset);
}

/*
 * sendSearchNotFoundReply()
 *
 * Makes the reply that tells the radio that a search found nothing.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendSearchNotFoundReply(WiresX* w)
{
    uint8_t data[70];

    memset(data, 0x00, sizeof(data));

    data[0] = w->seqNo;
    memcpy(data + 1U, ALL_RESP, 4U);

    data[5] = '0';
    data[6] = '1';

    memcpy(data + 7U, w->id, 5U);
    memcpy(data + 12U, w->node, 10U);

    data[22] = '1';
    data[23] = '0';
    data[24] = '0';
    data[25] = '0';
    data[26] = '0';
    data[27] = '0';

    data[28] = 0x0DU;

    data[29] = 0x03U;
    data[30] = addCRC(data, 30U);

    createReply(w, data, 31U);

    w->seqNo++;
}

/*
 * sendSearchReply()
 *
 * Makes the reply with a page of 20 of the reflectors found by the search.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendSearchReply(WiresX* w)
{
    uint8_t data[1100];
    char text[16];
    const Reflector** found;
    unsigned int count, total, n, offset;

    if (strlen(w->search) == 0U || w->reflectors->count == 0U)
    {
        sendSearchNotFoundReply(w);
        return;
    }

    found = malloc(w->reflectors->count * sizeof(found[0]));

    if (found == NULL)
        return;

    count = reflistSearch(w->reflectors, w->search, found, w->reflectors->count);

    if (count == 0U)
    {
        free(found);
        sendSearchNotFoundReply(w);
        return;
    }

    memset(data, 0x00, sizeof(data));

    data[0] = w->seqNo;
    memcpy(data + 1U, ALL_RESP, 4U);

    data[5] = '0';
    data[6] = '2';

    memcpy(data + 7U, w->id, 5U);
    memcpy(data + 12U, w->node, 10U);

    data[22] = '1';

    total = count > 999U ? 999U : count;
    n = count > w->start ? count - w->start : 0U;

    if (n > 20U)
        n = 20U;

    snprintf(text, sizeof(text), "%02u%03u", 20U, total);
    memcpy(data + 23U, text, 5U);

    data[28] = 0x0DU;

    offset = 29U;

    for (unsigned int j = 0U; j < n; j++, offset += 50U)
        putEntry(data + offset, '1', found[j + w->start]);

    free(found);

    finishList(w, data, offset);
}

/*
 * sendCategoryReply()
 *
 * Makes the reply with the reflectors of a category (up to 20).
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
static void sendCategoryReply(WiresX* w)
{
    uint8_t data[1100];
    char text[16];
    unsigned int n, offset;

    memset(data, 0x00, sizeof(data));

    data[0] = w->seqNo;
    memcpy(data + 1U, ALL_RESP, 4U);

    data[5] = '2';
    data[6] = '1';

    memcpy(data + 7U, w->id, 5U);
    memcpy(data + 12U, w->node, 10U);

    n = w->categoryCount;

    if (n > 20U)
        n = 20U;

    snprintf(text, sizeof(text), "%03u%03u", 20U, n);
    memcpy(data + 22U, text, 6U);

    data[28] = 0x0DU;

    offset = 29U;

    for (unsigned int j = 0U; j < n; j++, offset += 50U)
        putEntry(data + offset, '5', w->category[j]);

    finishList(w, data, offset);
}

/* ------------------------------------------------------------------------ */
/* Requests                                                                  */
/* ------------------------------------------------------------------------ */

/*
 * wxProcessConnect()
 *
 * Handles a request for a link that did not come from the radio (the
 * dashboard): the connect reply is sent after the timer.
 *
 * Parameters:
 *   w         - The handler.
 *   reflector - The reflector that is linked.
 *
 * Returns: nothing.
 */
void wxProcessConnect(WiresX* w, const Reflector* reflector)
{
    w->busy   = true;
    w->busyMs = 0U;

    w->reflector = reflector;

    w->status = WXSI_CONNECT;
    startTimer(w);
}

/*
 * wxProcessDisconnect()
 *
 * Handles an unlink: the reflector is forgotten and the disconnect reply is
 * sent after the timer.
 *
 * Parameters:
 *   w - The handler.
 *
 * Returns: nothing.
 */
void wxProcessDisconnect(WiresX* w)
{
    w->reflector = NULL;

    w->status = WXSI_DISCONNECT;
    startTimer(w);
}

/*
 * processConnect()
 *
 * Handles the connect request of the radio.  When the reflector is not in
 * the list, the failure is answered at once.
 *
 * Parameters:
 *   w    - The handler.
 *   data - The request after the command: the 5 digit reflector id.
 *
 * Returns: WX_CONNECT_YSF or WX_CONNECT_FCS for the kind of reflector,
 *   WX_DISCONNECT if it is not found.
 */
static WxStatus processConnect(WiresX* w, const uint8_t* data)
{
    char id[6];

    w->busy   = true;
    w->busyMs = 0U;

    memcpy(id, data, 5U);
    id[5] = '\0';

    w->reflector = reflistFindById(w->reflectors, id);

    if (w->reflector == NULL)
    {
        wxProcessDisconnect(w);
        sendConnectFailedReply(w);

        /* Keep state on the radio matched with the gateway */
        return WX_DISCONNECT;
    }

    w->status = WXSI_CONNECT;
    startTimer(w);

    return w->reflector->type == REFL_FCS ? WX_CONNECT_FCS : WX_CONNECT_YSF;
}

/*
 * wxProcess()
 *
 * Looks at a frame from the radio.  Data FR frames are collected into a
 * command; when it is complete and the check sum is good, the command (DX,
 * list, search, connect, disconnect or category) is handled.
 *
 * Parameters:
 *   w      - The handler.
 *   frame  - The frame, YSF_FRAME_LENGTH_BYTES bytes.
 *   source - Not used.
 *
 * Returns: WX_RECONNECT_CURRENT for a DX request, WX_CONNECT_YSF or
 *   WX_CONNECT_FCS for a connect, WX_DISCONNECT for a disconnect or failed
 *   connect, and WX_NONE for anything else.
 */
WxStatus wxProcess(WiresX* w, const uint8_t* frame, const uint8_t* source)
{
    YsfFich fich;
    uint8_t fn, ft;

    (void)source;

    if (!ysfFichDecode(&fich, frame))
        return WX_NONE;

    if (ysfFichGetDT(&fich) != YSF_DT_DATA_FR_MODE)
        return WX_NONE;

    if (ysfFichGetFI(&fich) != YSF_FI_COMMUNICATIONS)
        return WX_NONE;

    fn = ysfFichGetFN(&fich);

    if (fn == 0U)
        return WX_NONE;

    if (fn == 1U)
    {
        if (!ysfPayloadReadCsd(frame, 1U, w->command + 0U))
            return WX_NONE;
    }
    else
    {
        if (!ysfPayloadReadCsd(frame, 0U, w->command + (fn - 2U) * 40U + 20U))
            return WX_NONE;

        if (!ysfPayloadReadCsd(frame, 1U, w->command + (fn - 2U) * 40U + 40U))
            return WX_NONE;
    }

    ft = ysfFichGetFT(&fich);

    if (fn == ft)
    {
        bool valid = false;

        /* Find the end marker */
        unsigned int cmdLen = (fn - 1U) * 40U + 20U;

        for (unsigned int i = cmdLen; i > 0U; i--)
        {
            if (w->command[i] == 0x03U)
            {
                uint8_t crc = addCRC(w->command, i + 1U);

                if (crc == w->command[i + 1U])
                    valid = true;

                break;
            }
        }

        if (!valid)
            return WX_NONE;

        if (memcmp(w->command + 1U, DX_REQ, 3U) == 0)
        {
            processDX(w);
            return WX_RECONNECT_CURRENT;
        }
        else if (memcmp(w->command + 1U, ALL_REQ, 3U) == 0)
        {
            processAll(w, w->command + 5U);
            return WX_NONE;
        }
        else if (memcmp(w->command + 1U, CONN_REQ, 3U) == 0)
        {
            return processConnect(w, w->command + 5U);
        }
        else if (memcmp(w->command + 1U, DISC_REQ, 3U) == 0)
        {
            wxProcessDisconnect(w);
            return WX_DISCONNECT;
        }
        else if (memcmp(w->command + 1U, CAT_REQ, 3U) == 0)
        {
            processCategory(w, w->command + 5U);
            return WX_NONE;
        }
    }

    return WX_NONE;
}

/*
 * wxClock()
 *
 * Advances the timers: sends the reply when its timer has run out, sends one
 * queued frame every 90 ms and ends the busy state.
 *
 * Parameters:
 *   w  - The handler.
 *   ms - Time since the last call, in milliseconds.
 *
 * Returns: nothing.
 */
void wxClock(WiresX* w, uint32_t ms)
{
    if (w->timerRunning)
    {
        w->timerMs += ms;

        if (w->timerMs >= TIMER_MS)
        {
            switch (w->status)
            {
                case WXSI_DX:
                    sendDXReply(w);
                    break;
                case WXSI_ALL:
                    sendAllReply(w);
                    break;
                case WXSI_SEARCH:
                    sendSearchReply(w);
                    break;
                case WXSI_CONNECT:
                    sendConnectReply(w);
                    break;
                case WXSI_DISCONNECT:
                    sendDisconnectReply(w);
                    break;
                case WXSI_CATEGORY:
                    sendCategoryReply(w);
                    break;
                default:
                    break;
            }

            w->status       = WXSI_NONE;
            w->timerRunning = false;
        }
    }

    w->txMs += ms;

    if (w->txMs > TX_INTERVAL_MS)
    {
        if (w->qTail != w->qHead)
        {
            const uint8_t* e = w->queue[w->qTail];

            w->qTail = (w->qTail + 1U) % WX_TX_QUEUE;

            if (w->send != NULL)
                w->send(w->ctx, e + 1U, e[0] != 0U);
        }

        w->txMs = 0U;
    }

    if (w->busy)
    {
        w->busyMs += ms;

        if (w->busyMs >= BUSY_MS)
            w->busy = false;
    }
}

/*
 * wxCreateCommand()
 *
 * The frames of a request as a radio makes them (the same data FR frames as
 * the replies), for tests.
 *
 * Parameters:
 *   w      - The handler.
 *   data   - The data of the request.
 *   length - Number of bytes in data.
 *
 * Returns: nothing.
 */
void wxCreateCommand(WiresX* w, const uint8_t* data, unsigned int length)
{
    createReply(w, data, length);
}
