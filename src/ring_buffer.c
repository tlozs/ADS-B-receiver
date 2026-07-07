#include "ring_buffer.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

ring_buffer_t *ring_buffer_create(size_t samps_per_block) {    
    if (samps_per_block == 0) {
        fprintf(stderr, "ERROR: Cannot create a ring buffer with a zero-sized block.\n");
        return NULL;
    }
    ring_buffer_t *rb = calloc(1, sizeof(ring_buffer_t));
    if (!rb) {
        fprintf(stderr, "ERROR: Failed to allocate the ring buffer control structure.\n");
        return NULL;
    }

    rb->samps_per_block = samps_per_block;
    rb->is_shutting_down = false;
    
    // The NULL argument means "use default thread attributes"
    int mutex_rc = pthread_mutex_init(&rb->mutex, NULL);
    if (mutex_rc != 0) {
        fprintf(stderr, "ERROR: Failed to initialize the ring buffer mutex: %s\n", strerror(mutex_rc));
        goto cleanup_rb;
    }
    int cond_rc = pthread_cond_init(&rb->cond_ready, NULL);
    if (cond_rc != 0) {
        fprintf(stderr, "ERROR: Failed to initialize the ring buffer condition variable: %s\n", strerror(cond_rc));
        goto cleanup_mutex;
    }

    for (int i = 0; i < BLOCK_COUNT; i++) {
        // Multiply by 2 because complex IQ data has two 16-bit values per sample
        rb->ring[i].data = malloc(samps_per_block * 2 * sizeof(int16_t));
        if (!rb->ring[i].data) {
            fprintf(stderr, "ERROR: Failed to allocate a ring buffer data block.\n");
            // free previously allocated memory
            for (int j = 0; j < i; j++)
                free(rb->ring[j].data);
            goto cleanup_cond;
        }
        rb->ring[i].actual_sample_count = 0;
        rb->ring[i].is_valid = false;
    }
    return rb;

cleanup_cond:
    pthread_cond_destroy(&rb->cond_ready);
cleanup_mutex:
    pthread_mutex_destroy(&rb->mutex);
cleanup_rb:
    free(rb);

    return NULL;
}

void ring_buffer_destroy(ring_buffer_t *rb) {
    if (!rb) return;

    // Free the inner arrays first
    for (int i = 0; i < BLOCK_COUNT; i++)
        free(rb->ring[i].data);
    // Destroy the synchronization tools before freeing the main struct
    pthread_mutex_destroy(&rb->mutex);
    pthread_cond_destroy(&rb->cond_ready);
    
    free(rb);
}

iq_samps_block_t *ring_buffer_acquire_write(ring_buffer_t *rb) {
    assert(rb != NULL);

    pthread_mutex_lock(&rb->mutex);
    
    // Check if the current block is still being read by the consumer
    if (rb->ring[rb->write_idx].is_valid) {
        // Buffer Overrun! The CPU is too slow, tell the caller to drop the packet
        pthread_mutex_unlock(&rb->mutex);
        return NULL;
    }
    
    // It is empty and safe to write. Return the pointer, but keep is_valid = false
    iq_samps_block_t *block = &rb->ring[rb->write_idx];
    pthread_mutex_unlock(&rb->mutex);
    
    return block;
}

void ring_buffer_commit_write(ring_buffer_t *rb, size_t actual_samples) {
    assert(rb != NULL);

    pthread_mutex_lock(&rb->mutex);
    
    // Update the metadata, advance the index and wrap around
    rb->ring[rb->write_idx].actual_sample_count = actual_samples;
    rb->ring[rb->write_idx].is_valid = true;
    rb->write_idx = (rb->write_idx + 1) % BLOCK_COUNT;
    
    // Wake up the consumer thread
    pthread_cond_signal(&rb->cond_ready);
    pthread_mutex_unlock(&rb->mutex);
}

iq_samps_block_t *ring_buffer_acquire_read(ring_buffer_t *rb) {
    assert(rb != NULL);

    pthread_mutex_lock(&rb->mutex);
    
    // Go to sleep if the block is not ready yet and no shutdown signal is received
    while (!rb->ring[rb->read_idx].is_valid && !rb->is_shutting_down)
        pthread_cond_wait(&rb->cond_ready, &rb->mutex);

    // If we woke up because the fire alarm was pulled, bail out immediately
    if (rb->is_shutting_down) {
        pthread_mutex_unlock(&rb->mutex);
        return NULL; 
    }
    
    // Data is ready
    iq_samps_block_t *block = &rb->ring[rb->read_idx];
    pthread_mutex_unlock(&rb->mutex);
    return block;
}

void ring_buffer_commit_read(ring_buffer_t *rb) {
    assert(rb != NULL);
    
    // Mark the block as empty so the producer can overwrite it later and advance the index
    pthread_mutex_lock(&rb->mutex);
    rb->ring[rb->read_idx].is_valid = false;
    rb->read_idx = (rb->read_idx + 1) % BLOCK_COUNT;
    pthread_mutex_unlock(&rb->mutex);
}

void ring_buffer_abort(ring_buffer_t *rb) {
    if (!rb) return;
    // Broadcast wakes up all threads currently stuck in pthread_cond_wait
    pthread_mutex_lock(&rb->mutex);
    if (!rb->is_shutting_down) {
        rb->is_shutting_down = true;
        pthread_cond_broadcast(&rb->cond_ready);
    }
    pthread_mutex_unlock(&rb->mutex);
}
