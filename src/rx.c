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

#define EXECUTE_OR_GOTO(label, ...) \
    if (__VA_ARGS__) {              \
        return_code = EXIT_FAILURE; \
        goto label;                 \
    }

int init_usrp(rx_ctx_t *ctx) {

    // Define configuration values
    double freq          = 1090e6;
    double rate          = 2e6;
    double gain          = 40;
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
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_rx_streamer_max_num_samps(ctx->rx_streamer, &ctx->samps_per_buff))
    fprintf(stderr, "Buffer size in samples: %zu\n", ctx->samps_per_buff);

    // If we make it here, setup was successful. Exit the function.
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
    
    if (ctx->md) uhd_rx_metadata_free(&ctx->md);
    if (ctx->rx_streamer) uhd_rx_streamer_free(&ctx->rx_streamer);
    if (ctx->usrp) uhd_usrp_free(&ctx->usrp);
}

void do_rx_stream(rx_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {
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
        // Acquire write pointer from the ring buffer
        int16_t* buff = ring_buffer_acquire_write(rb)->data;
        void** buffs_ptr = (void**)&buff;

        size_t num_rx_samps = 0;
        
        // Receive directly into the shared memory
        if (uhd_rx_streamer_recv(ctx->rx_streamer, buffs_ptr, ctx->samps_per_buff, &ctx->md, 3.0, false, &num_rx_samps) != 0) {
            fprintf(stderr, "Streamer receive failed.\n");
            break;
        }
        
        // Commit the written samples for the consumer to read
        ring_buffer_commit_write(rb, num_rx_samps);

        // Error handling
        uhd_rx_metadata_error_code_t error_code;
        uhd_rx_metadata_error_code(ctx->md, &error_code);
        if (error_code != UHD_RX_METADATA_ERROR_CODE_NONE) {
            fprintf(stderr, "Error code 0x%x returned during streaming. Aborting.\n", error_code);
            break;
        }
    }
}
