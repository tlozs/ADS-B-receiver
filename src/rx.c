/*
 * Copyright 2026 Zsolt Gaál
 * 
 * Based on UHD RX example code:
 * Copyright 2015 Ettus Research LLC
 * Copyright 2018 Ettus Research, a National Instruments Company
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "rx.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define EXECUTE_OR_GOTO(label, ...) \
    if (__VA_ARGS__) {              \
        return_code = EXIT_FAILURE; \
        goto label;                 \
    }

int init_usrp(rx_ctx_t *ctx) {

    // Define configuration values
    double freq          = 1090e6;
    double rate          = 2e6;
    double gain          = 70;
    size_t channel       = 0;
    bool verbose         = false;
    int return_code      = EXIT_SUCCESS;
    char error_string[512];

    // Create other necessary structs
    uhd_tune_request_t tune_request = {
        .target_freq     = freq,
        .rf_freq_policy  = UHD_TUNE_REQUEST_POLICY_AUTO,
        .dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO,
    };
    uhd_tune_result_t tune_result;

    uhd_stream_args_t stream_args = {
        .cpu_format = "sc16",
        .otw_format = "sc16",
        .args = "",
        .channel_list = &channel,
        .n_channels = 1
    };

    // Create USRP
    fprintf(stderr, "Creating USRP...");
    uhd_usrp_make(&(ctx->usrp), "");

    // Create RX streamer and metadata
    EXECUTE_OR_GOTO(free_usrp, uhd_rx_streamer_make(&(ctx->rx_streamer)))
    EXECUTE_OR_GOTO(free_rx_streamer, uhd_rx_metadata_make(&(ctx->md)))

    // Set rate and check its value
    fprintf(stderr, "Setting RX Rate: %f...\n", rate);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_rate(ctx->usrp, rate, channel))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_rate(ctx->usrp, channel, &rate))
    fprintf(stderr, "Actual RX Rate: %f...\n", rate);

    // Set gain and check its value
    fprintf(stderr, "Setting RX Gain: %f dB...\n", gain);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_gain(ctx->usrp, gain, channel, ""))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_gain(ctx->usrp, channel, "", &gain))
    fprintf(stderr, "Actual RX Gain: %f...\n", gain);

    // Set frequency and check its value
    fprintf(stderr, "Setting RX frequency: %f MHz...\n", freq / 1e6);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_freq(ctx->usrp, &tune_request, channel, &tune_result))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_freq(ctx->usrp, channel, &freq))
    fprintf(stderr, "Actual RX frequency: %f MHz...\n", freq / 1e6);
    
    // Set up streamer
    stream_args.channel_list = &channel;
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_stream(ctx->usrp, &stream_args, ctx->rx_streamer))
    
    // Get the max number of samples to use as a base for buffer size
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_rx_streamer_max_num_samps(ctx->rx_streamer, &(ctx->samps_per_buff)))
    fprintf(stderr, "Buffer size in samples: %zu\n", ctx->samps_per_buff);

    ctx->trash_buffer = malloc(ctx->samps_per_buff * 2 * sizeof(int16_t));
    return 0;
    
free_rx_metadata:
    if (verbose) fprintf(stderr, "Cleaning up RX metadata.\n");
    uhd_rx_metadata_free(&(ctx->md));
free_rx_streamer:
    if (verbose) fprintf(stderr, "Cleaning up RX streamer.\n");
    uhd_rx_streamer_free(&(ctx->rx_streamer));
free_usrp:
    if (verbose) fprintf(stderr, "Cleaning up USRP.\n");
    if (return_code != EXIT_SUCCESS && ctx->usrp != NULL) {
        uhd_usrp_last_error(ctx->usrp, error_string, 512);
        fprintf(stderr, "USRP reported the following error: %s\n", error_string);
    }
    uhd_usrp_free(&(ctx->usrp));

    fprintf(stderr, (return_code ? "Failure\n" : "Success\n"));
    return return_code;
}

void teardown_usrp(rx_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->verbose) fprintf(stderr, "Tearing down SDR hardware...\n");
    
    if (ctx->trash_buffer) free(ctx->trash_buffer);
    if (ctx->md) uhd_rx_metadata_free(&(ctx->md));
    if (ctx->rx_streamer) uhd_rx_streamer_free(&(ctx->rx_streamer));
    if (ctx->usrp) uhd_usrp_free(&(ctx->usrp));
}

// Issues a stream command to the SDR and starts to receive data into the ring buffer
// until keep_running remains true.
static void do_rx_stream(rx_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {
    uhd_stream_cmd_t stream_cmd = {
        .stream_mode = UHD_STREAM_MODE_START_CONTINUOUS,
        .stream_now  = true
    };
    
    fprintf(stderr, "Issuing stream command.\n");
    if (uhd_rx_streamer_issue_stream_cmd(ctx->rx_streamer, &stream_cmd) != 0) {
        fprintf(stderr, "Failed to issue stream command.\n");
        return;
    }
    
    while (*keep_running) {
        // Attempt to acquire the ring buffer
        iq_samps_block_t *block = ring_buffer_acquire_write(rb);
        int16_t *target_buff = NULL;
        bool dropping_packet = false;
        
        // Route the data depending on buffer state
        if (!block) {
            // Buffer is full! Route the incoming radio waves to the trash
            target_buff = ctx->trash_buffer;
            dropping_packet = true;
            fprintf(stderr, "WARN: Ring buffer overrun! Dropping packet.\n");
        } else {
            // Buffer is good! Route the incoming radio waves to shared memory
            target_buff = block->data;
        }

        // Set up the pointer array for UHD
        void *buffs_ptr[1] = { target_buff };
        size_t num_rx_samps = 0;
        
        // Receive directly into the shared memory
        if (uhd_rx_streamer_recv(ctx->rx_streamer, buffs_ptr, ctx->samps_per_buff, &(ctx->md), 3.0, false, &num_rx_samps) != 0) {
            fprintf(stderr, "Streamer receive failed.\n");
            break;
        }
        
        // Commit the written samples for the consumer to read
        // Only commit the data if we didn't throw it in the trash
        if (!dropping_packet) {
            ring_buffer_commit_write(rb, num_rx_samps);
        }

        // Error handling
        uhd_rx_metadata_error_code_t error_code;
        uhd_rx_metadata_error_code(ctx->md, &error_code);
        if (error_code != UHD_RX_METADATA_ERROR_CODE_NONE) {
            fprintf(stderr, "Error code 0x%x returned during streaming. Aborting.\n", error_code);
            break;
        }
    }
}

// ============================================================================
// Thread Spawning Infrastructure
// ============================================================================

// An ugly payload struct, because pthread_create only accepts void* args
typedef struct {
    rx_ctx_t *ctx;
    ring_buffer_t *rb;
    volatile sig_atomic_t *keep_running;
} rx_thread_args_t;

// The unpacker function to call do_rx_stream with the correct arguments.
// It is explicitly marked 'static' so it is locked to this file.
static void *rx_thread_func(void *arg) {
    rx_thread_args_t *args = (rx_thread_args_t*)arg;
    do_rx_stream(args->ctx, args->rb, args->keep_running);
    free(args);
    return NULL;
}

pthread_t spawn_rx_thread(rx_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {
    // malloc is needed for the payload to survive the stack cleanup
    rx_thread_args_t *args = malloc(sizeof(rx_thread_args_t));
    args->ctx = ctx;
    args->rb = rb;
    args->keep_running = keep_running;

    pthread_t thread;
    if (pthread_create(&thread, NULL, rx_thread_func, args) != 0) {
        fprintf(stderr, "Failed to spawn rx thread.\n");
        free(args);
        // Return a null-equivalent thread ID on failure
        return 0;
    }
    return thread;
}
