/***************************************************************************
 *   Copyright (C) 2026 by Rick KD0OSS                                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 *                                                                         *
 *   Much of this code is from, based on or inspired by MMDVM created by   *
 *   Jonathan Naylor G4KLX                                                 *
 ***************************************************************************/

#include <RingBuffer.h>
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <tools.h>
#include <unistd.h>

#define BUFFER_LENGTH 500U
#define HOMEBREW_DATA_PACKET_LENGTH 55U

#define VERSION "2026-02-26"
#define BUFFER_SIZE 1024

pthread_t bmNetid;
pthread_t clientid;

int sockoutfd        = 0;
int clientPort       = 18400;
uint8_t modemId      = 1;
char modemName[8]    = "modem1";
uint32_t dmr_id      = 0;
char bmName[10]      = "";
char bmAddress[16]   = "127.0.0.1";
char bmPass[64]      = "passw0rd";
char callsign[9]     = "N0CALL";
char tx_state[4]     = "off";
char DMRHost[80]     = "127.0.0.1";
bool DMRBMDisconnect = false;
bool DMRBMConnected  = false;
bool bmPacketRdy     = false;
bool connected       = true;
bool debugM          = false;
uint8_t duration     = 0;
uint16_t bmPort      = 62031;
time_t start_time;

struct sockaddr_in servaddrin, servaddrout, cliaddr;

RingBuffer bmTxBuffer;
RingBuffer txBuffer;

pthread_mutex_t txBufMutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t bmTxBufMutex = PTHREAD_MUTEX_INITIALIZER; /* Protects bmTxBuffer */

// Homebrew protocol constants
#define HOMEBREW_RPTL "RPTL"        // Login request
#define HOMEBREW_RPTK "RPTK"        // Auth response
#define HOMEBREW_RPTC "RPTC"        // Config
#define HOMEBREW_RPTPING "RPTPING"  // Ping
#define HOMEBREW_DMRD "DMRD"        // DMR Data
#define HOMEBREW_RPTACK "RPTACK"    // ACK
#define HOMEBREW_MSTNAK "MSTNAK"    // NAK
#define HOMEBREW_MSTPONG "MSTPONG"  // Pong
#define HOMEBREW_MSTCL "MSTCL"      // Master close

const char* TYPE_DATA1      = "DMD1";
const char* TYPE_LOST1      = "DML1";
const char* TYPE_DATA2      = "DMD2";
const char* TYPE_LOST2      = "DML2";
const char* TYPE_START      = "DMST";
const char* TYPE_ABORT      = "DMAB";
const char* TYPE_ACK        = "ACK ";
const char* TYPE_NACK       = "NACK";
const char* TYPE_DISCONNECT = "DISC";
const char* TYPE_CONNECT    = "CONN";
const char* TYPE_STATUS     = "STAT";
const char* TYPE_COMMAND    = "COMM";

const uint8_t COMM_UPDATE_CONF = 0x04;

typedef enum
{
    STATUS_WAITING_CONNECT,
    STATUS_WAITING_LOGIN,
    STATUS_WAITING_AUTHORIZATION,
    STATUS_WAITING_CONFIG,
    STATUS_RUNNING
} network_status_t;

struct dmr_network
{
    int fd;
    struct sockaddr_in gateway_addr;
    uint32_t repeater_id;
    uint8_t seq;
    network_status_t status;
    uint64_t last_ping;
    uint64_t last_retry;
    uint32_t stream_id_counter;
    char modem_type[41];
    char software_version[41];
    char password[64];
    uint8_t salt[4];
    char callsign[9];
    char url[125];
    uint32_t rx_freq;
    uint32_t tx_freq;
    uint8_t power;
    uint8_t color_code;
    char slots;
    double latitude;
    double longitude;
    uint16_t height;
    char location[21];     // max 20 chars plus null
    char description[21];  // max 19 chars plus null
};

typedef enum
{
    NET_STREAM_START,
    NET_STREAM_END,
    NET_STREAM_LOST,
    NET_STREAM_IDLE
} stream_type_t;

typedef struct dmr_network dmr_network_t;

// DMR frame types for network transmission
typedef enum
{
    DMR_FRAME_VOICE_SYNC      = 0x15,
    DMR_FRAME_VOICE           = 0x10,
    DMR_FRAME_DATA_SYNC       = 0x20,
    DMR_FRAME_DATA            = 0x30,
    DMR_FRAME_VOICE_LC_HEADER = 0x40,
    DMR_FRAME_TERMINATOR      = 0x80,
} dmr_frame_type_t;

// Call type flags
#define DMR_CALL_TYPE_GROUP 0x00
#define DMR_CALL_TYPE_PRIVATE 0x03

dmr_network_t* g_dmr_network = NULL;

void* txThread(void* arg);

// error - wrapper for perror
void error(char* msg)
{
    perror(msg);
    exit(1);
}

// Wait for 'delay' miroseconds
void delay(uint32_t delay)
{
    struct timespec req, rem;
    req.tv_sec  = 0;
    req.tv_nsec = delay * 1000;
    nanosleep(&req, &rem);
};

// Print debug data.
// From MMDVM project by Jonathan Naylor G4KLX.
void dump(char* text, unsigned char* data, unsigned int length)
{
    struct timespec ts;
    uint64_t milliseconds;
    unsigned int offset = 0U;
    unsigned int i;

    // Get the current time with nanosecond precision
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
    {
        perror("clock_gettime");
        return;
    }

    // Convert to milliseconds
    milliseconds = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    fprintf(stdout, "%s: %llu\n", text, milliseconds);

    while (length > 0U)
    {
        unsigned int bytes = (length > 16U) ? 16U : length;

        fprintf(stdout, "%04X:  ", offset);

        for (i = 0U; i < bytes; i++) fprintf(stdout, "%02X ", data[offset + i]);

        for (i = bytes; i < 16U; i++) fputs("   ", stdout);

        fputs("   *", stdout);

        for (i = 0U; i < bytes; i++)
        {
            unsigned char c = data[offset + i];

            if (isprint(c))
                fputc(c, stdout);
            else
                fputc('.', stdout);
        }

        fputs("*\n", stdout);

        offset += 16U;

        if (length >= 16U)
            length -= 16U;
        else
            length = 0U;
    }
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

static bool send_login(dmr_network_t* net)
{
    uint8_t buffer[8];
    memcpy(buffer, HOMEBREW_RPTL, 4);
    buffer[4] = (net->repeater_id >> 24) & 0xFF;
    buffer[5] = (net->repeater_id >> 16) & 0xFF;
    buffer[6] = (net->repeater_id >> 8) & 0xFF;
    buffer[7] = net->repeater_id & 0xFF;

    ssize_t sent = sendto(net->fd, buffer, 8, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    fprintf(stderr, "[DMR Network] Sent login request (RPTL)\n");
    return (sent == 8);
}

dmr_network_t* dmr_network_open(const char* host, uint16_t port, uint32_t repeater_id, const char* password, const char* callsign, uint32_t rx_freq, uint32_t tx_freq, double latitude, double longitude, uint16_t height, uint8_t power, uint8_t color_code, const char slots)
{
    dmr_network_t* net = calloc(1, sizeof(*net));
    if (!net) return NULL;

    net->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (net->fd < 0)
    {
        free(net);
        return NULL;
    }

    /* Increase receive buffer to prevent packet loss during bursts */
    int rcvbuf = 256 * 1024; /* 256KB buffer */
    setsockopt(net->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    set_nonblocking(net->fd);

    memset(&net->gateway_addr, 0, sizeof(net->gateway_addr));
    net->gateway_addr.sin_family = AF_INET;
    net->gateway_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &net->gateway_addr.sin_addr) <= 0)
    {
        close(net->fd);
        free(net);
        return NULL;
    }

    net->repeater_id       = repeater_id;
    net->seq               = 0;
    net->status            = STATUS_WAITING_CONNECT;
    net->last_ping         = 0;
    net->last_retry        = 0;
    net->stream_id_counter = 0;

    if (password && strlen(password) > 0)
    {
        strncpy(net->password, password, sizeof(net->password) - 1);
    }
    else
    {
        strcpy(net->password, "passw0rd");  // Default password
    }

    // Set callsign (default to N0CALL if not provided)
    if (callsign && strlen(callsign) > 0)
    {
        strncpy(net->callsign, callsign, sizeof(net->callsign) - 1);
    }
    else
    {
        snprintf(net->callsign, sizeof(net->callsign), "N0CALL");
    }

    net->rx_freq    = rx_freq;
    net->tx_freq    = tx_freq;
    net->power      = power;
    net->color_code = color_code;
    net->slots      = slots;
    net->latitude   = latitude;
    net->longitude  = longitude;
    net->height     = height;
    net->url[0]     = 0;
    strcpy(net->modem_type, "MMDVM");
    strcpy(net->software_version, "20260319");
    strcpy(net->location, "New London, IA");
    strcpy(net->description, "KD0OSS Repeater");

    // Start login process
    send_login(net);
    net->status     = STATUS_WAITING_LOGIN;
    net->last_retry = now_ms();

    fprintf(stderr, "[DMR Network] Connecting to %s:%d (ID: %u, Callsign: %s)\n", host, port, repeater_id, net->callsign);

    return net;
}

int dmr_network_is_connected(const dmr_network_t* net)
{
    return (net && net->status == STATUS_RUNNING) ? 1 : 0;
}

static bool send_authorization(dmr_network_t* net)
{
    // Build: salt (4 bytes) + password
    size_t pass_len     = strlen(net->password);
    uint8_t* hash_input = malloc(4 + pass_len);
    memcpy(hash_input, net->salt, 4);
    memcpy(hash_input + 4, net->password, pass_len);

    // Compute SHA256
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(hash_input, 4 + pass_len, hash);
    free(hash_input);

    // Build RPTK packet: "RPTK" + ID + hash
    uint8_t buffer[40];
    memcpy(buffer, HOMEBREW_RPTK, 4);
    buffer[4] = (net->repeater_id >> 24) & 0xFF;
    buffer[5] = (net->repeater_id >> 16) & 0xFF;
    buffer[6] = (net->repeater_id >> 8) & 0xFF;
    buffer[7] = net->repeater_id & 0xFF;
    memcpy(buffer + 8, hash, 32);

    ssize_t sent = sendto(net->fd, buffer, 40, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    fprintf(stderr, "[DMR Network] Sent authorization (RPTK with SHA256)\n");
    return (sent == 40);
}

static bool send_config(dmr_network_t* net)
{
    char buffer[302];
    memset(buffer, 0, sizeof(buffer));

    memcpy(buffer, HOMEBREW_RPTC, 4);
    buffer[4] = (net->repeater_id >> 24) & 0xFF;
    buffer[5] = (net->repeater_id >> 16) & 0xFF;
    buffer[6] = (net->repeater_id >> 8) & 0xFF;
    buffer[7] = net->repeater_id & 0xFF;

    char latitude[20U];
    sprintf(latitude, "%08f", net->latitude);

    char longitude[20U];
    sprintf(longitude, "%09f", net->longitude);

    unsigned int power = net->power;
    if (power > 99U)
        power = 99U;

    int height = net->height;
    if (height > 999)
        height = 999;

    // Format: callsign(8) rxfreq(9) txfreq(9) power(2) colorcode(2) lat(8) lon(9) height(3) location(20) desc(19) slots(1) url(124) version(40) software(40)
    snprintf(buffer + 8, 294,
             "%-8.8s%09u%09u%02u%02u%8.8s%9.9s%03d%-20.20s%-19.19s%c%-124.124s%-40.40s%-40.40s",
             net->callsign,
             net->rx_freq,
             net->tx_freq,
             net->power,
             net->color_code,
             latitude,
             longitude,
             height,
             net->location,
             net->description,
             net->slots,
             net->url,
             net->software_version,
             net->modem_type);

    ssize_t sent = sendto(net->fd, buffer, 302, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));
    fprintf(stderr, "[%s]\n", buffer + 8);
    fprintf(stderr, "[DMR Network] Sent config (RPTC)\n");
    return (sent == 302);
}

static bool send_ping(dmr_network_t* net)
{
    uint8_t buffer[11];
    memcpy(buffer, HOMEBREW_RPTPING, 7);
    buffer[7]  = (net->repeater_id >> 24) & 0xFF;
    buffer[8]  = (net->repeater_id >> 16) & 0xFF;
    buffer[9]  = (net->repeater_id >> 8) & 0xFF;
    buffer[10] = net->repeater_id & 0xFF;

    ssize_t sent = sendto(net->fd, buffer, 11, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    return (sent == 11);
}

void dmr_network_keepalive(dmr_network_t* net)
{
    if (!net) return;

    uint64_t now = now_ms();

    // Handle retries
    if (net->status != STATUS_RUNNING && (now - net->last_retry) >= 10000)
    {
        switch (net->status)
        {
            case STATUS_WAITING_LOGIN:
                send_login(net);
                break;
            case STATUS_WAITING_AUTHORIZATION:
                send_authorization(net);
                break;
            case STATUS_WAITING_CONFIG:
                send_config(net);
                break;
            default:
                break;
        }
        net->last_retry = now;
    }

    // Send ping when running
    if (net->status == STATUS_RUNNING && (now - net->last_ping) >= 10000)
    {
        send_ping(net);
        net->last_ping = now;
    }

    // Process incoming packets
    uint8_t buffer[512];
    ssize_t n = recv(net->fd, buffer, sizeof(buffer), 0);

    if (n > 0)
    {
        if (memcmp(buffer, HOMEBREW_RPTACK, 6) == 0)
        {
            switch (net->status)
            {
                case STATUS_WAITING_LOGIN:
                    fprintf(stderr, "[DMR Network] Received RPTACK - login accepted\n");
                    memcpy(net->salt, buffer + 6, 4);
                    send_authorization(net);
                    net->status     = STATUS_WAITING_AUTHORIZATION;
                    net->last_retry = now;
                    break;
                case STATUS_WAITING_AUTHORIZATION:
                    fprintf(stderr, "[DMR Network] Received RPTACK - auth accepted\n");
                    send_config(net);
                    net->status     = STATUS_WAITING_CONFIG;
                    net->last_retry = now;
                    break;
                case STATUS_WAITING_CONFIG:
                    fprintf(stderr, "[DMR Network] Received RPTACK - config accepted\n");
                    fprintf(stderr, "[DMR Network] *** CONNECTED AND RUNNING ***\n");
                    net->status    = STATUS_RUNNING;
                    net->last_ping = now;
                    uint8_t buf[4];
                    buf[0] = 0x61;
                    buf[1] = 0x00;
                    buf[2] = 0x11;
                    buf[3] = 0x04;
                    pthread_mutex_lock(&txBufMutex);
                    RingBuffer_addData(&txBuffer, buf, 4);
                    RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_CONNECT, 4);
                    RingBuffer_addData(&txBuffer, (uint8_t*)bmName, 9);
                    pthread_mutex_unlock(&txBufMutex);
                    break;
                default:
                    break;
            }
        }
        else if (memcmp(buffer, HOMEBREW_MSTNAK, 6) == 0)
        {
            fprintf(stderr, "[DMR Network] Received MSTNAK - connection failed!\n");
            // Start login process
            net->seq               = 0;
            net->status            = STATUS_WAITING_CONNECT;
            net->last_ping         = 0;
            net->last_retry        = 0;
            net->stream_id_counter = 0;
            send_login(net);
            net->status     = STATUS_WAITING_LOGIN;
            net->last_retry = now_ms();
        }
        else if (memcmp(buffer, HOMEBREW_MSTPONG, 7) == 0)
        {
            // Pong received, connection alive
        }
        else if (memcmp(buffer, HOMEBREW_DMRD, 4) == 0)
        {
            // Data packet - handled by dmr_network_recv
        }
    }
}

int dmr_network_send(dmr_network_t* net, const uint8_t* dmr_payload, uint8_t slot, uint32_t src_id, uint32_t dst_id, uint8_t is_group, uint8_t frame_type, uint32_t stream_id)
{
    if (!net || !dmr_payload || net->status != STATUS_RUNNING) return -1;

    uint8_t packet[55];
    memset(packet, 0, sizeof(packet));

    memcpy(packet, HOMEBREW_DMRD, 4);
    packet[4] = net->seq++;

    packet[5] = (src_id >> 16) & 0xFF;
    packet[6] = (src_id >> 8) & 0xFF;
    packet[7] = src_id & 0xFF;

    packet[8]  = (dst_id >> 16) & 0xFF;
    packet[9]  = (dst_id >> 8) & 0xFF;
    packet[10] = dst_id & 0xFF;

    packet[11] = (net->repeater_id >> 24) & 0xFF;
    packet[12] = (net->repeater_id >> 16) & 0xFF;
    packet[13] = (net->repeater_id >> 8) & 0xFF;
    packet[14] = net->repeater_id & 0xFF;

    packet[15] = (slot == 2) ? 0x80 : 0x00;
    packet[15] |= is_group ? 0x00 : 0x40;

    if (frame_type == 0x20)  // DMR_FRAME_VOICE_SYNC
    {
        packet[15] |= 0x10;
    }
    else if (frame_type < 6)  // DMR_FRAME_VOICE
    {
        packet[15] |= ((net->seq - 2) % 6U);
    }
    else
    {
        packet[15] |= 0x20 | frame_type;
    }

    packet[16] = (stream_id >> 24) & 0xFF;
    packet[17] = (stream_id >> 16) & 0xFF;
    packet[18] = (stream_id >> 8) & 0xFF;
    packet[19] = stream_id & 0xFF;

    memcpy(&packet[20], dmr_payload, 33);
    packet[53] = 0;  // BER
    packet[54] = 0;  // RSSI

    // Debug: dump packet for first few frames
    static int dump_count = 0;
    //    if (dump_count < 3)
    {
        char hex[256] = {0};
        for (int i = 0; i < 55; i++)
        {
            sprintf(hex + strlen(hex), "%02X ", packet[i]);
        }
        fprintf(stderr, "[DMR Network TX] DMRD packet (%d bytes): %s\n", 55, hex);
        fprintf(stderr, "[DMR Network TX] Seq=%u Src=%u Dst=%u Slot=%u Group=%u FT=0x%02X Stream=0x%08X\n",
                packet[4], src_id, dst_id, slot, is_group, frame_type, stream_id);
        dump_count++;
    }
    if (frame_type == 0x42)
        dump_count = 0;

    ssize_t sent = sendto(net->fd, packet, 55, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    return (sent == 55) ? 0 : -1;
}

int dmr_network_recv(dmr_network_t* net, uint8_t* dmr_payload, uint32_t* stream_id, uint8_t* frame_number, uint8_t* slot, uint32_t* src_id, uint32_t* dst_id, uint8_t* is_group, uint8_t* frame_type)
{
    if (!net || net->status != STATUS_RUNNING) return -2;

    uint8_t packet[256];
    ssize_t n = recv(net->fd, packet, sizeof(packet), 0);

    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        error((char*)"ERROR: BM connection closed remotely.");
        return -2;
    }
    //   fprintf(stderr, "Returned: %d", n);
    if (n < 55 || memcmp(packet, HOMEBREW_DMRD, 4) != 0) return -1;

    *src_id       = ((uint32_t)packet[5] << 16) | ((uint32_t)packet[6] << 8) | packet[7];
    *dst_id       = ((uint32_t)packet[8] << 16) | ((uint32_t)packet[9] << 8) | packet[10];
    *stream_id    = (uint32_t)((packet[16] << 24) | ((packet[17] << 16) & 0xff0000) | ((packet[18] << 8) & 0xff00) | (packet[19] & 0xff));
    *frame_number = (uint8_t)packet[4];
    *slot         = (packet[15] & 0x80) ? 2 : 1;
    *is_group     = (packet[15] & 0x40) ? 0 : 1;
    *frame_type   = packet[15] & 0x3f;

    if (*frame_type == 0x21 || *frame_type == 0x22)
        fprintf(stderr, "Slot: %u  Src: %u  Dst: %u  Is Grp: %u  F-type: %02X\n", *slot, *src_id, *dst_id, *is_group, *frame_type);

    memcpy(dmr_payload, &packet[20], 33);
    return 0;
}

// Subscribe to a talkgroup on BrandMeister
int dmr_network_subscribe_tg(dmr_network_t* net, uint32_t talkgroup, uint8_t slot)
{
    if (!net || net->status != STATUS_RUNNING) return -1;

    char buffer[100];
    memset(buffer, 0, sizeof(buffer));

    memcpy(buffer, "RPTO", 4);  // Options packet
    buffer[4] = (net->repeater_id >> 24) & 0xFF;
    buffer[5] = (net->repeater_id >> 16) & 0xFF;
    buffer[6] = (net->repeater_id >> 8) & 0xFF;
    buffer[7] = net->repeater_id & 0xFF;

    // Options string: "TS1=talkgroup" or "TS2=talkgroup"
    sprintf(buffer + 8, "TS%d=%u", slot, talkgroup);

    int len = 8 + strlen(buffer + 8);

    ssize_t sent = sendto(net->fd, buffer, len, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    if (sent > 0)
    {
        fprintf(stderr, "[DMR Network] Subscribed to TG %u on TS%u\n", talkgroup, slot);
        return 0;
    }

    return -1;
}

// Unsubscribe from current TG on a timeslot
int dmr_network_unsubscribe_tg(dmr_network_t* net, uint8_t slot)
{
    if (!net || net->status != STATUS_RUNNING) return -1;

    char buffer[100];
    memset(buffer, 0, sizeof(buffer));

    memcpy(buffer, "RPTO", 4);
    buffer[4] = (net->repeater_id >> 24) & 0xFF;
    buffer[5] = (net->repeater_id >> 16) & 0xFF;
    buffer[6] = (net->repeater_id >> 8) & 0xFF;
    buffer[7] = net->repeater_id & 0xFF;

    // Empty TG = disconnect
    sprintf(buffer + 8, "TS%d=", slot);

    int len = 8 + strlen(buffer + 8);

    ssize_t sent = sendto(net->fd, buffer, len, 0, (struct sockaddr*)&net->gateway_addr, sizeof(net->gateway_addr));

    if (sent > 0)
    {
        fprintf(stderr, "[DMR Network] Unsubscribed from TS%u\n", slot);
        return 0;
    }

    return -1;
}

void dmr_network_close(dmr_network_t* net)
{
    if (!net) return;
    close(net->fd);
    free(net);
}

void* startDMRNetwork(void* arg)
{
    uint8_t dmr_payload[33];
    uint8_t slot               = 0;
    uint32_t src_id            = 0;
    uint32_t dst_id            = 0;
    uint32_t stream_id         = 0;
    uint32_t curr_stream_id[2] = {0, 0};
    uint8_t frame_number       = 0;
    uint8_t is_group           = 0;
    uint8_t frame_type         = 0;
    uint32_t rx_freq           = 400000000;
    uint32_t tx_freq           = 400000000;
    double latitude            = 0.0;
    double longitude           = 0.0;
    uint16_t height            = 0;
    uint8_t power              = 0;
    uint8_t color_code         = 0;
    char slots[2]              = "3";
    int sockfd                 = (intptr_t)arg;
    bool frame_started[2]      = {false, false};

    uint32_t packet_count[2] = {0, 0}; /* Track all received packets */
    uint32_t current_src[2]  = {0, 0}; /* Track current transmission source */

    char tmp[15];
    readHostConfig(modemName, "config", "rxFrequency", tmp);
    rx_freq = atol(tmp);
    readHostConfig(modemName, "config", "txFrequency", tmp);
    tx_freq = atol(tmp);
    readHostConfig(modemName, "config", "latitude", tmp);
    latitude = atof(tmp);
    readHostConfig(modemName, "config", "longitude", tmp);
    longitude = atof(tmp);
    readHostConfig(modemName, "DMR", "colorCode", tmp);
    color_code = atoi(tmp);
    readHostConfig(modemName, "DMR", "slots", slots);

    g_dmr_network = dmr_network_open(bmAddress, bmPort, dmr_id, bmPass, callsign, rx_freq, tx_freq, latitude, longitude, height, power, color_code, slots[0]);

    if (g_dmr_network)
    {
        if (dmr_network_is_connected(g_dmr_network))
        {
            printf("DMR network connected\n");
        }
        else
        {
            printf("DMR network opened (waiting for connection)\n");
        }
    }
    else
    {
        fprintf(stderr, "Failed to initialize DMR network\n");
    }

    pthread_t txid;
    int err = pthread_create(&(txid), NULL, &txThread, (void*)(intptr_t)sockfd);
    if (err != 0)
        fprintf(stderr, "Can't create DMR TX thread :[%s]", strerror(err));
    else
    {
        if (debugM)
            fprintf(stderr, "DMR TX thread created successfully\n");
    }

    while (connected)
    {
        // Check if we're actually connected
        static int last_status = 0;
        int current_status     = dmr_network_is_connected(g_dmr_network);
        if (current_status != last_status)
        {
            fprintf(stderr, "[DMR Network RX] Connection status changed: %d\n", current_status);
            last_status = current_status;
        }

        // Try to receive frame from network
        int result = dmr_network_recv(g_dmr_network, dmr_payload, &stream_id, &frame_number, &slot, &src_id, &dst_id, &is_group, &frame_type);

        if (result == 0)
        {
            packet_count[slot - 1]++;

            /* Detect new transmission (source changed) */
            if (src_id != current_src[slot - 1])
            {
                if (current_src[slot - 1] != 0)
                {
                    fprintf(stderr, "\n[DMR Network RX] === END SLOT%u From %u (received %u packets) ===\n\n",
                            slot, current_src[slot - 1], packet_count[slot - 1]);
                }
                current_src[slot - 1]  = src_id;
                packet_count[slot - 1] = 0;
                fprintf(stderr, "\n[DMR Network RX] === START SLOT%u From %u ===\n", slot, src_id);
            }

            fprintf(stderr, "[DMR Network RX] [SLOT%u PKT#%u FRM#%u] ", slot, packet_count[slot - 1], frame_number);

            /* Only dump first 3 packets per transmission - dump() is VERY slow! */
            //      if (tx_packet_count <= 3)
            //          dump((char*)"DMR", dmr_payload, 33);

            if (frame_type == 0x21 && !frame_started[slot - 1])
            {
                fprintf(stderr, "Stream Start\n");
                uint8_t buf[9];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x2a;
                buf[3] = 0x04;
                buf[8] = 0x41;
                if (slot == 1)
                    memcpy(buf + 4, TYPE_DATA1, 4);
                else
                    memcpy(buf + 4, TYPE_DATA2, 4);
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 9);
                RingBuffer_addData(&txBuffer, dmr_payload, 33);
                pthread_mutex_unlock(&txBufMutex);
                curr_stream_id[slot - 1] = stream_id;
                frame_started[slot - 1]  = true;
            }
            else if (frame_type == 0x22 && frame_started[slot - 1])
            {
                fprintf(stderr, "Stream End (EOT)\n");
                fprintf(stderr, "=== END From %u (received %u packets) ===\n\n", current_src[slot - 1], packet_count[slot - 1]);
                uint8_t buf[9];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x2a;
                buf[3] = 0x04;
                buf[8] = 0x42;
                if (slot == 1)
                    memcpy(buf + 4, TYPE_DATA1, 4);
                else
                    memcpy(buf + 4, TYPE_DATA2, 4);
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 9);
                RingBuffer_addData(&txBuffer, dmr_payload, 33);
                pthread_mutex_unlock(&txBufMutex);
                curr_stream_id[slot - 1] = 0;
                frame_started[slot - 1]  = false;
                current_src[slot - 1]    = 0;
                packet_count[slot - 1]   = 0;
            }
            else if (frame_type == 0x10 && frame_started[slot - 1] && curr_stream_id[slot - 1] == stream_id)
            {
                fprintf(stderr, "Voice Sync\n");
                uint8_t buf[9];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x2a;
                buf[3] = 0x04;
                buf[8] = 0x20;
                if (slot == 1)
                    memcpy(buf + 4, TYPE_DATA1, 4);
                else
                    memcpy(buf + 4, TYPE_DATA2, 4);
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 9);
                RingBuffer_addData(&txBuffer, dmr_payload, 33);
                pthread_mutex_unlock(&txBufMutex);
            }
            else if (frame_type < 0x10 && frame_started[slot - 1] && curr_stream_id[slot - 1] == stream_id)
            {
                fprintf(stderr, "Voice\n");
                uint8_t buf[9];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x2a;
                buf[3] = 0x04;
                buf[8] = frame_type & 0x0f;
                if (slot == 1)
                    memcpy(buf + 4, TYPE_DATA1, 4);
                else
                    memcpy(buf + 4, TYPE_DATA2, 4);
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 9);
                RingBuffer_addData(&txBuffer, dmr_payload, 33);
                pthread_mutex_unlock(&txBufMutex);
            }
            else
                fprintf(stderr, "Data type: %02X\n", frame_type);

            // Don't delay - immediately try to get next packet
            //    continue;
        }

        if (RingBuffer_dataSize(&bmTxBuffer) >= 49)
        {
            uint8_t buf[49];
            bool isGroup = false;
            pthread_mutex_lock(&bmTxBufMutex);
            RingBuffer_peek(&bmTxBuffer, buf, 9);
            if (memcmp(buf + 4, TYPE_DATA1, 4) == 0)
            {
                if (!frame_started[0] && buf[8] == 0x41)
                {
                    curr_stream_id[0]  = (rand() % 0xffffffff) + 1;
                    frame_started[0]   = true;
                    g_dmr_network->seq = 0;
                    if (debugM)
                        fprintf(stderr, "[DMR Network] Slot 1 TX stream started.\n");
                }
                else if (frame_started[0] && buf[8] == 0x41)
                {
                    RingBuffer_getData(&bmTxBuffer, buf, 49);
                    pthread_mutex_unlock(&bmTxBufMutex);
                    continue;
                }
                RingBuffer_getData(&bmTxBuffer, buf, 49);
                pthread_mutex_unlock(&bmTxBufMutex);

                isGroup = buf[48];
                uint32_t src_id;
                uint32_t dst_id;
                dst_id = buf[42] << 16;
                dst_id |= buf[43] << 8;
                dst_id |= buf[44];
                src_id = buf[45] << 16;
                src_id |= buf[46] << 8;
                src_id |= buf[47];
                dmr_network_send(g_dmr_network, buf + 9, 1, src_id, dst_id, isGroup, buf[8], curr_stream_id[0]);
                if (buf[8] == 0x42)
                {
                    frame_started[0]  = false;
                    curr_stream_id[0] = 0;
                    if (debugM)
                        fprintf(stderr, "[DMR Network] Slot 1 TX stream stopped.\n");
                }
            }
            else if (memcmp(buf + 4, TYPE_DATA2, 4) == 0)
            {
                if (!frame_started[1] && buf[8] == 0x41)
                {
                    curr_stream_id[1]  = (rand() % 0xffffffff) + 1;
                    frame_started[1]   = true;
                    g_dmr_network->seq = 0;
                    if (debugM)
                        fprintf(stderr, "[DMR Network] Slot 2 TX stream started.\n");
                }
                else if (frame_started[1] && buf[8] == 0x41)
                {
                    RingBuffer_getData(&bmTxBuffer, buf, 49);
                    pthread_mutex_unlock(&bmTxBufMutex);
                    continue;
                }
                RingBuffer_getData(&bmTxBuffer, buf, 49);
                pthread_mutex_unlock(&bmTxBufMutex);

                isGroup = buf[48];
                uint32_t src_id;
                uint32_t dst_id;
                dst_id = buf[42] << 16;
                dst_id |= buf[43] << 8;
                dst_id |= buf[44];
                src_id = buf[45] << 16;
                src_id |= buf[46] << 8;
                src_id |= buf[47];
                dmr_network_send(g_dmr_network, buf + 9, 2, src_id, dst_id, isGroup, buf[8], curr_stream_id[1]);
                if (buf[8] == 0x42)
                {
                    frame_started[1]  = false;
                    curr_stream_id[1] = 0;
                    if (debugM)
                        fprintf(stderr, "[DMR Network] Slot 2 TX stream stopped.\n");
                }
            }
        }
        else if (result == 0)
        {
            continue;
        }
        else if (result == -1)
        {
            // No data available - good time to send keepalive
            dmr_network_keepalive(g_dmr_network);

            // Sleep briefly
            delay(1000);
        }
        else
        {
            // Error - also send keepalive to maintain connection
            dmr_network_keepalive(g_dmr_network);

            static int error_count = 0;
            if (error_count < 5)
            {
                fprintf(stderr, "[DMR Network RX] Error receiving: result=%d\n", result);
                error_count++;
            }
            frame_started[0] = false;
            frame_started[1] = false;
            delay(10000);
        }
    }

    fprintf(stderr, "DMR network thread exited.\n");
    int iRet  = 200;
    connected = false;
    pthread_exit(&iRet);
    return 0;
}

// Thread used to send data to DMR host.
void* txThread(void* arg)
{
    int sockfd    = (intptr_t)arg;
    uint16_t loop = 0;

    while (connected)
    {
        delay(100);
        loop++;

        if (loop > 50)
        {
            if (RingBuffer_dataSize(&txBuffer) >= 5)
            {
                pthread_mutex_lock(&txBufMutex);
                uint8_t buf[1];
                RingBuffer_peek(&txBuffer, buf, 1);
                if (buf[0] != 0x61)
                {
                    fprintf(stderr, "TX invalid header.\n");
                    pthread_mutex_unlock(&txBufMutex);
                    continue;
                }
                uint8_t byte[3];
                uint16_t len = 0;
                RingBuffer_peek(&txBuffer, byte, 3);
                len = (byte[1] << 8) + byte[2];
                ;
                if (RingBuffer_dataSize(&txBuffer) >= len)
                {
                    uint8_t buf[len];
                    RingBuffer_getData(&txBuffer, buf, len);
                    if (write(sockfd, buf, len) < 0)
                    {
                        pthread_mutex_unlock(&txBufMutex);
                        fprintf(stderr, "ERROR: remote disconnect\n");
                        break;
                    }
                }
                pthread_mutex_unlock(&txBufMutex);
            }

            loop = 0;
        }
    }
    fprintf(stderr, "TX thread exited.\n");
    int iRet  = 500;
    connected = false;
    pthread_exit(&iRet);
    return NULL;
}

// Start up connection to DMR service.
void* startClient(void* arg)
{
    int sockfd = 0;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE] = {0};
    char hostAddress[80];
    strcpy(hostAddress, (char*)arg);
    // Create socket file descriptor
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("Socket creation error");
        exit(EXIT_FAILURE);
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }

    int on = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // Prepare the sockaddr_in structure
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(clientPort);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, hostAddress, &serv_addr.sin_addr) <= 0)
    {  // Connect to localhost
        perror("Invalid address/ Address not supported");
        exit(EXIT_FAILURE);
    }

    // Connect to the server
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
    {
        if (errno != EINPROGRESS)
        {
            perror("connection failed");
            exit(EXIT_FAILURE);
        }
    }

    sleep(1);
    printf("Connected to DMR host.\n");

    int err = pthread_create(&(bmNetid), NULL, &startDMRNetwork, (void*)(intptr_t)sockfd);
    if (err != 0)
    {
        fprintf(stderr, "Can't create DMR Network thread :[%s]", strerror(err));
        int iRet  = 100;
        connected = false;
        pthread_exit(&iRet);
    }
    else
    {
        if (debugM)
            fprintf(stderr, "DMR Network thread created successfully\n");
    }

    ssize_t len      = 0;
    uint8_t offset   = 0;
    uint16_t respLen = 0;
    uint8_t typeLen  = 0;
    char source[7];

    while (connected)
    {
        int len = read(sockfd, buffer, 1);
        if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            error((char*)"ERROR: DMR host connection closed remotely.");
            break;
        }

        if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            delay(5);
        }

        if (len != 1)
        {
            fprintf(stderr, "DMR_Gateway: error when reading from DMR host, errno=%d\n", errno);
            break;
        }

        if (buffer[0] != 0x61)
        {
            fprintf(stderr, "DMR_Gateway: unknown byte from DMR host, 0x%02X\n", buffer[0]);
            continue;
        }

        offset = 0;
        while (offset < 3)
        {
            len = read(sockfd, buffer + 1 + offset, 3 - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        respLen = (buffer[1] << 8) + buffer[2];

        offset += 1;
        while (offset < respLen)
        {
            len = read(sockfd, buffer + offset, respLen - offset);
            if (len == 0) break;
            if (len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                delay(5);
            else
                offset += len;
        }

        if (len == 0)
            break;

        typeLen = buffer[3];
        uint8_t type[typeLen];
        memcpy(type, buffer + 4, typeLen);

        if (debugM)
            dump((char*)"DMR Service data:", (unsigned char*)buffer, respLen);

        if (memcmp(type, TYPE_COMMAND, typeLen) == 0)
        {
            if (buffer[8] == COMM_UPDATE_CONF)
            {
                char tmp[10];
                readHostConfig(modemName, "DMR", "ID", tmp);
                dmr_id = atol(tmp);
                readHostConfig(modemName, "main", "callsign", callsign);
                uint8_t buf[4];
                buf[0] = 0x61;
                buf[1] = 0x00;
                buf[2] = 0x08;
                buf[3] = 0x04;
                pthread_mutex_lock(&txBufMutex);
                RingBuffer_addData(&txBuffer, buf, 4);
                RingBuffer_addData(&txBuffer, (uint8_t*)TYPE_COMMAND, 4);
                pthread_mutex_unlock(&txBufMutex);
            }
        }
        else if (memcmp(type, TYPE_CONNECT, typeLen) == 0)
        {
            /*      if (!DSTARReflConnected)
             *                 {
             *                     bzero(source, 7);
             *                     bzero(DSTARName, 8);
             *                     bzero(DSTARModule, 2);
             *                     memcpy(source, buffer + 8, 6);
             *                     memcpy(DSTARName, buffer + 14, 7);
             *                     memcpy(DSTARModule, buffer + 21, 1);
             *                     fprintf(stderr, "Name: %s  Mod: %s\n", DSTARName, DSTARModule);
             *
             *                     char cBuffer[40];
             *                     uint8_t ucBytes[12];
             *                     sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "        ", source, "TEST");
             *                     memcpy(cBuffer + 16, DSTARName, 6);
             *                     cBuffer[22] = DSTARModule[0];
             *                     cBuffer[23] = 'L';
             *                     memset(ucBytes, 0, 9);
             *                     ucBytes[9]  = 0x55;
             *                     ucBytes[10] = 0x2d;
             *                     ucBytes[11] = 0x16;
             *                     sendToGw(cBuffer, ucBytes, true, true);
        } */
        }
        else if (memcmp(type, TYPE_DISCONNECT, typeLen) == 0)
        {
            /*            if (DSTARReflConnected)
             *                       {
             *                           char cBuffer[40];
             *                           uint8_t ucBytes[12];
             *                           sprintf(cBuffer, "%8s%8s%8s%8s%4s", rpt2Call, rpt1Call, "       U", source, "TEST");
             *                           memset(ucBytes, 0, 9);
             *                           ucBytes[9]  = 0x55;
             *                           ucBytes[10] = 0x2d;
             *                           ucBytes[11] = 0x16;
             *                           sendToGw(cBuffer, ucBytes, true, true);
        } */
        }
        else if (memcmp(type, TYPE_STATUS, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_DATA1, typeLen) == 0)
        {
            pthread_mutex_lock(&bmTxBufMutex);
            RingBuffer_addData(&bmTxBuffer, (uint8_t*)buffer, 49);
            pthread_mutex_unlock(&bmTxBufMutex);
        }
        else if (memcmp(type, TYPE_LOST1, typeLen) == 0)
        {
        }
        else if (memcmp(type, TYPE_DATA2, typeLen) == 0)
        {
            pthread_mutex_lock(&bmTxBufMutex);
            RingBuffer_addData(&bmTxBuffer, (uint8_t*)buffer, 49);
            pthread_mutex_unlock(&bmTxBufMutex);
        }
        else if (memcmp(type, TYPE_LOST2, typeLen) == 0)
        {
        }

        delay(500);
    }

    fprintf(stderr, "Client thread exited.\n");
    close(sockfd);
    int iRet  = 100;
    connected = false;
    pthread_exit(&iRet);
}

int main(int argc, char** argv)
{
    bool daemon = false;
    int ret;
    int c;

    while ((c = getopt(argc, argv, ":m:dxv")) != -1)
    {
        switch (c)
        {
            case 'm':
            {
                modemId = atoi(optarg);
                if (modemId < 1 || modemId > 10)
                {
                    fprintf(stderr, "Invalid modem number.\n");
                    return 1;
                }
                sprintf(modemName, "modem%d", modemId);
                clientPort = 18100 + modemId - 1;
                fprintf(stderr, "Modem name: %s\n", modemName);
            }
            break;
            case ':':
                fprintf(stderr, "Option 'm' requires modem number.\n");
                return 1;
            case 'd':
                daemon = true;
                break;
            case 'v':
                fprintf(stdout, "DMR_Gateway: version " VERSION "\n");
                return 0;
            case 'x':
                debugM = true;
                break;
            default:
                fprintf(stderr, "Usage: DMR_Gateway [-m modem_number (1-10)] [-d] [-v] [-x]\n");
                return 1;
        }
    }

    if (daemon)
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            fprintf(stderr, "DMR_Gateway: error in fork(), exiting\n");
            return 1;
        }

        // If this is the parent, exit
        if (pid > 0)
            return 0;

        // We are the child from here onwards
        setsid();

        umask(0);
    }

    /* Initialize RingBuffers */
    RingBuffer_Init(&bmTxBuffer, 800);
    RingBuffer_Init(&txBuffer, 800);

    bzero(bmName, 10);
    readHostConfig(modemName, "DMR", "BM_Name", bmName);
    readHostConfig(modemName, "DMR", "BM_pass", bmPass);
    char tmp[15];
    readHostConfig(modemName, "DMR", "ID", tmp);
    dmr_id = atol(tmp);
    readHostConfig(modemName, "DMR", "BM_port", tmp);
    bmPort = atol(tmp);
    readHostConfig(modemName, "main", "callsign", callsign);
    readHostConfig(modemName, "DMR", "BM_Address", bmAddress);

    int err = pthread_create(&(clientid), NULL, &startClient, DMRHost);
    if (err != 0)
    {
        fprintf(stderr, "Can't create DMR host thread :[%s]", strerror(err));
        return 1;
    }
    else
    {
        if (debugM)
            fprintf(stderr, "DMR host thread created successfully\n");
    }

    while (1)
    {
        delay(50000);
        if (!connected)
            break;
    }

    if (g_dmr_network)
    {
        dmr_network_close(g_dmr_network);
    }

    /* Cleanup RingBuffers */
    RingBuffer_Destroy(&bmTxBuffer);
    RingBuffer_Destroy(&txBuffer);

    fprintf(stderr, "DMR Gateway terminated.\n");
    logError(modemName, "main", "DMR Gateway terminated.");
    return 0;
}
