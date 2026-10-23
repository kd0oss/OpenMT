/* Created by Rick KD0OSS */
#include "RingBuffer.h"

/*
 * RingBuffer_Init()
 *
 * Allocates and zeroes the storage of a ring buffer and empties it.  The
 * ring buffer is not thread safe; callers share one between threads only
 * under their own mutex.  Asserts that the length is not zero.
 *
 * Parameters:
 *   self   - Ring buffer to operate on.
 *   length - Size of the storage in bytes.  One byte is always kept unused,
 *            so the buffer holds at most length - 1 bytes.
 *
 * Returns: nothing.
 */
void RingBuffer_Init(RingBuffer* self, unsigned int length)
{
    assert(length > 0U);
    self->m_length = length;
    self->m_iPtr = 0U;
    self->m_oPtr = 0U;
    self->m_buffer = (rb_sample_t*)malloc(length * sizeof(rb_sample_t));
    
    if (self->m_buffer) {
        memset(self->m_buffer, 0, length * sizeof(rb_sample_t));
    }
}

/*
 * RingBuffer_Destroy()
 *
 * Frees the storage of a ring buffer and sets its pointer to NULL.  Does
 * nothing if the pointer is already NULL, for example after a failed
 * allocation in RingBuffer_Init().
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: nothing.
 */
void RingBuffer_Destroy(RingBuffer* self)
{
    if (self->m_buffer) {
        free(self->m_buffer);
        self->m_buffer = NULL;
    }
}

/*
 * RingBuffer_addData()
 *
 * Appends bytes to the buffer.  If there is not strictly more free space
 * than nSamples, nothing is added and the whole buffer is emptied, so that
 * old data never blocks new data.
 *
 * Parameters:
 *   self     - Ring buffer to operate on.
 *   buffer   - Bytes to add.
 *   nSamples - Number of bytes to add.
 *
 * Returns: true if the bytes were stored, false if there was not enough room
 *   and the buffer was cleared.
 */
bool RingBuffer_addData(RingBuffer* self, const rb_sample_t* buffer, unsigned int nSamples)
{
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

/*
 * RingBuffer_getData()
 *
 * Removes bytes from the front of the buffer.
 *
 * Parameters:
 *   self     - Ring buffer to operate on.
 *   buffer   - Output buffer for the bytes.
 *   nSamples - Number of bytes to remove.
 *
 * Returns: true if the bytes were copied out, false if the buffer holds
 *   fewer than nSamples (nothing is removed then).
 */
bool RingBuffer_getData(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples)
{
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

/*
 * RingBuffer_peek()
 *
 * Copies bytes from the front of the buffer without removing them.
 *
 * Parameters:
 *   self     - Ring buffer to operate on.
 *   buffer   - Output buffer for the bytes.
 *   nSamples - Number of bytes to look at.
 *
 * Returns: true if the bytes were copied, false if the buffer holds fewer
 *   than nSamples.
 */
bool RingBuffer_peek(RingBuffer* self, rb_sample_t* buffer, unsigned int nSamples)
{
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

/*
 * RingBuffer_clear()
 *
 * Discards all data and zeroes the storage.
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: nothing.
 */
void RingBuffer_clear(RingBuffer* self)
{
    self->m_iPtr = 0U;
    self->m_oPtr = 0U;
    memset(self->m_buffer, 0, self->m_length * sizeof(rb_sample_t));
}

/*
 * RingBuffer_freeSpace()
 *
 * Reports how many more bytes the buffer can take.
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: Free space in bytes.  An empty buffer reports its full length,
 *   although addData() needs strictly more free space than the number of bytes
 *   added.
 */
unsigned int RingBuffer_freeSpace(const RingBuffer* self)
{
    unsigned int len = self->m_length;
    if (self->m_oPtr > self->m_iPtr)
        len = self->m_oPtr - self->m_iPtr;
    else if (self->m_iPtr > self->m_oPtr)
        len = self->m_length - (self->m_iPtr - self->m_oPtr);

    return (len > self->m_length) ? 0U : len;
}

/*
 * RingBuffer_dataSize()
 *
 * Reports how many bytes are waiting in the buffer.
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: Number of stored bytes.
 */
unsigned int RingBuffer_dataSize(const RingBuffer* self)
{
    return self->m_length - RingBuffer_freeSpace(self);
}

/*
 * RingBuffer_hasSpace()
 *
 * Tests whether a block of a given size can be added.
 *
 * Parameters:
 *   self   - Ring buffer to operate on.
 *   length - Number of bytes to be added.
 *
 * Returns: true if there is strictly more free space than length.
 */
bool RingBuffer_hasSpace(const RingBuffer* self, unsigned int length)
{
    return RingBuffer_freeSpace(self) > length;
}

/*
 * RingBuffer_hasData()
 *
 * Tests whether the buffer holds any data.
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: true if at least one byte is stored.
 */
bool RingBuffer_hasData(const RingBuffer* self)
{
    return self->m_oPtr != self->m_iPtr;
}

/*
 * RingBuffer_isEmpty()
 *
 * Tests whether the buffer is empty.
 *
 * Parameters:
 *   self - Ring buffer to operate on.
 *
 * Returns: true if no bytes are stored.
 */
bool RingBuffer_isEmpty(const RingBuffer* self)
{
    return self->m_oPtr == self->m_iPtr;
}
