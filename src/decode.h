#pragma once

#include "ring_buffer.h"
#include <mode-s.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>

// Context necessary for decoding data from the ring buffer.
typedef struct {
    mode_s_t *mode_s;
    uint8_t *buff_downsampled;
    uint16_t *mag;
} decode_ctx_t;

// Sets up the context data, allocating memory for mode_s and the decode buffers.
// Explicitly enforces decoding parameters to:
// - Drop corrupted packets before the callback
// - Allow safe 1-bit mathematical repairs
// - Disable hallucinated noise detection
int init_decode(decode_ctx_t *ctx, size_t samps_per_buff);

// Clears up the context data from memory.
void teardown_decode(decode_ctx_t *ctx);

// Acts as a wrapper function for do_decode, spawning it in its own dedicated thread.
// Hides the ugly payload crafting and wrapper function of the thread creation logic.
int spawn_decode_thread(decode_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running, pthread_t *out_thread);
