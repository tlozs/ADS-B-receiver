#pragma once

#include "ring_buffer.h"
#include <mode-s.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <pthread.h>


typedef struct {
    mode_s_t* mode_s;
    ring_buffer_t* rb;
    size_t samps_per_buff;
    volatile sig_atomic_t *keep_running;
    uint8_t* buff_downsampled;
    uint16_t* mag;
} decode_ctx_t;

// Prepares heap memory buffers and initializes the libmodes decoding engine.
void init_decode(decode_ctx_t* ctx, size_t samps_per_buff);

// @brief Infinite worker loop that reads data from the ring buffer and runs the DSP pipeline.
void do_decode(decode_ctx_t* ctx);

// @brief Callback function triggered automatically whenever a valid aircraft message is decoded.
void on_message(mode_s_t* mode_s, struct mode_s_msg* mm);

// @brief Frees all dynamically allocated memory buffers to prevent system memory leaks.
void teardown_decode(decode_ctx_t* ctx);

//@brief Spawns a background POSIX thread to run the decoder loop without blocking the main program.
pthread_t spawn_decode_thread(decode_ctx_t* ctx, ring_buffer_t* rb, volatile sig_atomic_t *keep_running);