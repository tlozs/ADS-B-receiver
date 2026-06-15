#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// The size of the ring buffer.
#define BLOCK_COUNT 16

// Holds a number of IQ data samples.
typedef struct {
    // The buffer containing IQ data samples.
    int16_t *data;
    // The actual number of samples stored in the buffer, as the buffer can be only partially filled.
    size_t actual_sample_count;
    // The flag signaling whether the data inside is ready to be processed.
    bool is_valid;
} iq_samps_block_t;

// The ring buffer storing blocks of IQ data samples with additional metadata for management.
typedef struct {
    // The buffer of buffers, holding blocks of IQ data.
    iq_samps_block_t ring[BLOCK_COUNT];
    // The buffer size of each block.
    size_t block_size;
    uint32_t write_idx;
    uint32_t read_idx;
    pthread_mutex_t mutex;
    pthread_cond_t cond_ready;
} ring_buffer_t;

// Constructs the ring buffer, allocating its memory and initializing the mutex and condition variable.
ring_buffer_t* ring_buffer_create(size_t block_size);
void ring_buffer_destroy(ring_buffer_t *rb);

// Checks if there is space in the ring buffer.
// Returns a pointer to the first available block, without marking the block as valid.
iq_samps_block_t* ring_buffer_acquire_write(ring_buffer_t *rb);

// Updates the ring buffer metadata after writing to it via the address acquire_write() returned.
// This function has to be used after calling ring_buffer_acquire_write first.
void ring_buffer_commit_write(ring_buffer_t *rb, size_t actual_samples);

// Sends the thread to sleep until a valid block is ready to read.
// Returns a pointer to the next block with valid data inside.
iq_samps_block_t* ring_buffer_acquire_read(ring_buffer_t *rb);

// Updates the ring buffer metadata after reading from it via the address acquire_read() returned.
// This function has to be used after calling ring_buffer_acquire_read first.
void ring_buffer_commit_read(ring_buffer_t *rb);