#pragma once

#include "ring_buffer.h"
#include "radar_state.h"
#include <uhd.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

// Context necessary for receiving stream data through the SDR.
typedef struct {
    uhd_usrp_handle usrp;
    uhd_rx_streamer_handle rx_streamer;
    uhd_rx_metadata_handle md;
    double default_gain;
    size_t channel;
    size_t samps_per_buff;
    int16_t *trash_buffer;
} rx_ctx_t;

// Initializes the SDR with predefined configuration values and sets up the context data.
int init_usrp(rx_ctx_t *ctx);

// Clears up the context data from memory.
void teardown_usrp(rx_ctx_t *ctx);

// Tries a set of gain levels in order to determine where the most number of CPR packets can be decoded.
// Saves the results to a config file to retain it when restarting the code.
void run_auto_tune(rx_ctx_t *rx_ctx, radar_state_ctx_t *radar_ctx);

// Acts as a wrapper function for do_rx_stream, spawning it in its own dedicated thread.
// Hides the ugly payload crafting and wrapper function of the thread creation logic.
int spawn_rx_thread(rx_ctx_t *ctx, ring_buffer_t *rb, atomic_bool *keep_running, pthread_t *out_thread);
