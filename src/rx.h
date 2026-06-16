#pragma once

#include "ring_buffer.h"
#include <uhd.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

// Context neccesairy for recieving stream data through the SDR
typedef struct {
    uhd_usrp_handle usrp;
    uhd_rx_streamer_handle rx_streamer;
    uhd_rx_metadata_handle md;
    size_t samps_per_buff;
    bool verbose;
} rx_ctx_t;

// Initializes the SDR with predefined configuration values and sets up the context data.
int init_usrp(rx_ctx_t *ctx);

// Clears up the context data from memory.
void teardown_usrp(rx_ctx_t *ctx);

// Issues a stream command to the SDR and starts to receive data into the ring buffer until keep_running remains true.
void do_rx_stream(rx_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running);