#include "RingBuffer.h"

void RingBuffer_Init(RingBuffer* self, unsigned int length) {
    assert(length > 0U);
    self->m_length = length;
    self->m_iPtr = 0U;
    self->m_oPtr = 0U;
    self->m_buffer = (rb_sample_t*)malloc(length * sizeof(rb_sample_t));
    
    if (self->m_buffer) {
        memset(self->m_buffer, 0, length * sizeof(rb_sample_t));
    }
}

void RingBuffer_Destroy(RingBuffer* self) {
    if (self->m_buffer) {
        free(self->m_buffer);
        self->m_buffer = NULL;
    }
}

bool RingBuffer_addData(RingBuffer* self, const rb_sample_t* buffer, unsigned int nSamples) {
    if (nSamples >= RingBuffer_freeSpace(self)) {
        RingBuffer_clear(self);
        return false;
    }

    for (unsigned int i = 0U; i < nSamples; i++) {
        self->m_buffer[self->m_iPtr++] = buffer[i];
        if (self->m_iPtr == self->m_length)
            self->m_iPtr = 0U;
    }
    return true;
}

bool RingBuffer_getData(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples) {
    if (RingBuffer_dataSize(self) < nSamples) {
        return false;
    }

    for (unsigned int i = 0U; i < nSamples; i++) {
        buffer[i] = self->m_buffer[self->m_oPtr++];
        if (self->m_oPtr == self->m_length)
            self->m_oPtr = 0U;
    }
    return true;
}

bool RingBuffer_peek(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples) {
    if (RingBuffer_dataSize(self) < nSamples) {
        return false;
    }

    unsigned int ptr = self->m_oPtr;
    for (unsigned int i = 0U; i < nSamples; i++) {
        buffer[i] = self->m_buffer[ptr++];
        if (ptr == self->m_length)
            ptr = 0U;
    }
    return true;
}

void RingBuffer_clear(RingBuffer* self) {
    self->m_iPtr = 0U;
    self->m_oPtr = 0U;
    memset(self->m_buffer, 0, self->m_length * sizeof(rb_sample_t));
}

unsigned int RingBuffer_freeSpace(const RingBuffer* self) {
    unsigned int len = self->m_length;
    if (self->m_oPtr > self->m_iPtr)
        len = self->m_oPtr - self->m_iPtr;
    else if (self->m_iPtr > self->m_oPtr)
        len = self->m_length - (self->m_iPtr - self->m_oPtr);

    return (len > self->m_length) ? 0U : len;
}

unsigned int RingBuffer_dataSize(const RingBuffer* self) {
    return self->m_length - RingBuffer_freeSpace(self);
}

bool RingBuffer_hasSpace(const RingBuffer* self, unsigned int length) {
    return RingBuffer_freeSpace(self) > length;
}

bool RingBuffer_hasData(const RingBuffer* self) {
    return self->m_oPtr != self->m_iPtr;
}

bool RingBuffer_isEmpty(const RingBuffer* self) {
    return self->m_oPtr == self->m_iPtr;
}
