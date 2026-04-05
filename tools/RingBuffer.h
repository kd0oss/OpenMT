#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

// Change this to the data type you need (int, double, uint8_t, etc.)
typedef uint8_t rb_sample_t;

typedef struct {
    unsigned int m_length;
    rb_sample_t* m_buffer;
    unsigned int m_iPtr;
    unsigned int m_oPtr;
} RingBuffer;

// Lifecycle
void RingBuffer_Init(RingBuffer* self, unsigned int length);
void RingBuffer_Destroy(RingBuffer* self);

// Data Operations
bool RingBuffer_addData(RingBuffer* self, const rb_sample_t* buffer, unsigned int nSamples);
bool RingBuffer_getData(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples);
bool RingBuffer_peek(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples);
void RingBuffer_clear(RingBuffer* self);

// Status
unsigned int RingBuffer_freeSpace(const RingBuffer* self);
unsigned int RingBuffer_dataSize(const RingBuffer* self);
bool RingBuffer_hasSpace(const RingBuffer* self, unsigned int length);
bool RingBuffer_hasData(const RingBuffer* self);
bool RingBuffer_isEmpty(const RingBuffer* self);

#endif
