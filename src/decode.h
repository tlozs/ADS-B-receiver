#pragma once

#include "ring_buffer.h"
#include <mode-s.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>

typedef struct {
    mode_s_t* mode_s;
    ring_buffer_t* rb;
    size_t samps_per_buff;
    volatile sig_atomic_t *keep_running;
    uint8_t* buff_downsampled;
    uint16_t* mag;
} decode_ctx_t;

void init_decode(decode_ctx_t* ctx, ring_buffer_t* rb, volatile sig_atomic_t *keep_running);
void do_decode(decode_ctx_t* ctx);
void teardown_decode(decode_ctx_t* ctx);