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
#include "radar_state.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#define GAIN_CONFIG_FILE "sdr_gain.conf"

#define EXECUTE_OR_GOTO(label, ...) \
    if (__VA_ARGS__) {              \
        return_code = EXIT_FAILURE; \
        goto label;                 \
    }

static double load_saved_gain(double fallback_default) {
    double saved_gain = fallback_default;
    FILE *f = fopen(GAIN_CONFIG_FILE, "r");
    
    if (f != NULL) {
        if (fscanf(f, "%lf", &saved_gain) != 1) {
            fprintf(stderr, "WARNING: '" GAIN_CONFIG_FILE "' is corrupted. Using default.\n");
            saved_gain = fallback_default;
        }
        fclose(f);
    } else {
        fprintf(stderr, "No '" GAIN_CONFIG_FILE "' found. Using default.\n");
    }
    
    return saved_gain;
}

int init_usrp(rx_ctx_t *ctx) {
    assert(ctx != NULL);

    // Define configuration values
    ctx->channel         = 0;
    ctx->default_gain    = 29.0;
    double freq          = 1090e6;
    double rate          = 2e6;
    double gain          = load_saved_gain(ctx->default_gain);
    int return_code      = EXIT_SUCCESS;
    char error_string[512];

    // Create the UHD configuration structures needed by the receiver.
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
        .channel_list = &ctx->channel,
        .n_channels = 1
    };

    // Create USRP
    EXECUTE_OR_GOTO(return_error, uhd_usrp_make(&ctx->usrp, ""))

    // Create RX streamer and metadata
    EXECUTE_OR_GOTO(free_usrp, uhd_rx_streamer_make(&ctx->rx_streamer))
    EXECUTE_OR_GOTO(free_rx_streamer, uhd_rx_metadata_make(&ctx->md))

    // Set rate and check its value
    fprintf(stderr, "Setting RX Rate: %f...\n", rate);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_rate(ctx->usrp, rate, ctx->channel))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_rate(ctx->usrp, ctx->channel, &rate))
    fprintf(stderr, "Actual RX Rate: %f...\n", rate);

    // Set gain and check its value
    fprintf(stderr, "Setting RX Gain: %f dB...\n", gain);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_gain(ctx->usrp, gain, ctx->channel, ""))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_gain(ctx->usrp, ctx->channel, "", &gain))
    fprintf(stderr, "Actual RX Gain: %f...\n", gain);

    // Set frequency and check its value
    fprintf(stderr, "Setting RX frequency: %f MHz...\n", freq / 1e6);
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_set_rx_freq(ctx->usrp, &tune_request, ctx->channel, &tune_result))
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_freq(ctx->usrp, ctx->channel, &freq))
    fprintf(stderr, "Actual RX frequency: %f MHz...\n", freq / 1e6);
    
    // Set up streamer
    stream_args.channel_list = &ctx->channel;
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_usrp_get_rx_stream(ctx->usrp, &stream_args, ctx->rx_streamer))
    
    // Get the max number of samples to use as a base for buffer size
    EXECUTE_OR_GOTO(free_rx_metadata, uhd_rx_streamer_max_num_samps(ctx->rx_streamer, &ctx->samps_per_buff))
    fprintf(stderr, "Buffer size in samples: %zu\n", ctx->samps_per_buff);

    ctx->trash_buffer = malloc(ctx->samps_per_buff * 2 * sizeof(int16_t));
    if (ctx->trash_buffer)
        return EXIT_SUCCESS;
    else {
        fprintf(stderr, "ERROR: Failed to allocate the SDR trash buffer.\n");
        return_code = EXIT_FAILURE;
    }
    
free_rx_metadata:
    uhd_rx_metadata_free(&ctx->md);
free_rx_streamer:
    uhd_rx_streamer_free(&ctx->rx_streamer);
free_usrp:
    if (return_code != EXIT_SUCCESS && ctx->usrp != NULL) {
        uhd_usrp_last_error(ctx->usrp, error_string, 512);
        fprintf(stderr, "ERROR: USRP reported the following error: %s\n", error_string);
    }
    uhd_usrp_free(&ctx->usrp);
return_error:

    return return_code;
}

void teardown_usrp(rx_ctx_t *ctx) {
    if (!ctx) return;

    free(ctx->trash_buffer);
    if (ctx->md) uhd_rx_metadata_free(&ctx->md);
    if (ctx->rx_streamer) uhd_rx_streamer_free(&ctx->rx_streamer);
    if (ctx->usrp) uhd_usrp_free(&ctx->usrp);
}

void run_auto_tune(rx_ctx_t *rx_ctx, radar_state_ctx_t *radar_ctx) {
    assert(rx_ctx != NULL);
    assert(radar_ctx != NULL);

    // Define the sweep range
    double test_gains[] = {20.0, 23.0, 26.0, 29.0, 32.0, 35.0, 38.0, 41.0, 44.0, 47.0};
    int num_gains = sizeof(test_gains) / sizeof(test_gains[0]);
    
    double best_gain = rx_ctx->default_gain;
    double max_ppp = 0;

    fprintf(stderr, "\n============================================================\n");
    fprintf(stderr, " INITIATING SDR AUTO-TUNE SEQUENCE (5m per step, 50m total)\n");
    fprintf(stderr, "============================================================\n");

    for (int i = 0; i < num_gains; i++) {
        double current_gain = test_gains[i];
        
        // Command the UHD hardware to change gain on the fly
        uhd_usrp_set_rx_gain(rx_ctx->usrp, current_gain, rx_ctx->channel, "");
        uhd_usrp_get_rx_gain(rx_ctx->usrp, rx_ctx->channel, "", &current_gain);

        // Clear the aircraft repository to have a blank slate for every benchmark
        // and reset the RAM counter for this specific test block
        clear_radar_state(radar_ctx);
        atomic_store(&radar_ctx->valid_telemetry_count, 0);
        atomic_store(&radar_ctx->aircraft_created_count, 0);

        fprintf(stderr, "Testing gain %.2f dB... ", current_gain);
        
        // Put the main thread to sleep.
        // The rx_thread and decode_thread continue running at maximum speed
        sleep(300); 

        // Harvest the results
        int messages_caught = atomic_load(&radar_ctx->valid_telemetry_count);
        int new_aircrafts_created = atomic_load(&radar_ctx->aircraft_created_count);
        double packets_per_plane = (new_aircrafts_created > 0) ? 
                                   (messages_caught / (double)new_aircrafts_created) : 0.0;
        fprintf(stderr, "Captured %4d valid CPR packets of %3d planes, resulting in %.2f packets per plane on average.\n", 
                messages_caught, 
                new_aircrafts_created,
                packets_per_plane
        );

        // Track the peak of the plateau
        if (max_ppp < packets_per_plane) {
            max_ppp = packets_per_plane;
            best_gain = current_gain;
        }
    }

    fprintf(stderr, "==================================================\n");
    fprintf(stderr, " AUTO-TUNE COMPLETE. LOCKING GAIN TO: %.2f dB\n", best_gain);
    fprintf(stderr, "==================================================\n\n");

    // Lock the hardware to the winning value before returning
    uhd_usrp_set_rx_gain(rx_ctx->usrp, best_gain, rx_ctx->channel, "");

    FILE *f = fopen(GAIN_CONFIG_FILE, "w");
    if (f != NULL) {
        fprintf(f, "%.2f\n", best_gain);
        fclose(f);
        fprintf(stderr, "Saved optimized gain to '" GAIN_CONFIG_FILE "'.\n");
    } else {
        fprintf(stderr, "WARNING: Could not write to '" GAIN_CONFIG_FILE "'. Tune will not persist.\n");
    }
}

// Issues a stream command to the SDR and receives data into the ring buffer
// until shutdown is requested.
static void do_rx_stream(rx_ctx_t *ctx, ring_buffer_t *rb, atomic_bool *keep_running) {
    assert(ctx != NULL);
    assert(rb != NULL);
    assert(keep_running != NULL);

    fprintf(stderr, "RX thread started.\n");

    uhd_stream_cmd_t stream_cmd = {
        .stream_mode = UHD_STREAM_MODE_START_CONTINUOUS,
        .stream_now  = true
    };
    
    if (uhd_rx_streamer_issue_stream_cmd(ctx->rx_streamer, &stream_cmd) != 0) {
        fprintf(stderr, "ERROR: Failed to issue stream command.\n");
        goto fatal_error;
    }
    
    while (atomic_load(keep_running)) {
        // Attempt to acquire the ring buffer
        iq_samps_block_t *block = ring_buffer_acquire_write(rb);
        int16_t *target_buff = NULL;
        bool dropping_packet = false;
        
        // Route the data depending on buffer state
        if (!block) {
            // Buffer is full. Route the incoming IQ samples to the fallback buffer.
            target_buff = ctx->trash_buffer;
            dropping_packet = true;
            fprintf(stderr, "WARNING: Ring buffer overrun; dropping packet.\n");
        // Buffer is good! Route the incoming radio waves to shared memory
        } else
            target_buff = block->data;

        // Set up the pointer array for UHD
        void *buffs_ptr[1] = { target_buff };
        size_t num_rx_samps = 0;
        
        // Receive directly into the shared memory
        if (uhd_rx_streamer_recv(ctx->rx_streamer, buffs_ptr, ctx->samps_per_buff, &ctx->md, 3.0, false, &num_rx_samps) != 0) {
            fprintf(stderr, "ERROR: Streamer receive API failed.\n");
            goto fatal_error;
        }
        
        // Evaluate hardware status for any error
        uhd_rx_metadata_error_code_t error_code;
        uhd_rx_metadata_error_code(ctx->md, &error_code);
        if (error_code == UHD_RX_METADATA_ERROR_CODE_TIMEOUT) {
            fprintf(stderr, "ERROR: RX Stream timeout. Hardware disconnected?\n");
            goto fatal_error;
        // Treat other codes (like OUT_OF_SEQUENCE drops) as warnings, let the loop recover
        } else if (error_code != UHD_RX_METADATA_ERROR_CODE_NONE)
            fprintf(stderr, "WARNING: UHD stream reported error code 0x%x. Recovering...\n", error_code);
        
        // Commit the written samples for the consumer to read
        // Only commit the data if we didn't throw it in the trash
        // and if there is data available
        if (!dropping_packet && 0 < num_rx_samps)
            ring_buffer_commit_write(rb, num_rx_samps);
    }

    return;
fatal_error:
    atomic_store(keep_running, false);
    ring_buffer_abort(rb);
}

// ============================================================================
// Thread Spawning Infrastructure
// ============================================================================

// An ugly payload struct, because pthread_create only accepts void* args.
typedef struct {
    rx_ctx_t *ctx;
    ring_buffer_t *rb;
    atomic_bool *keep_running;
} rx_thread_args_t;

// The unpacker function to call do_rx_stream with the correct arguments.
// It is explicitly marked 'static' so it is locked to this file.
static void *rx_thread_func(void *arg) {
    rx_thread_args_t *args = (rx_thread_args_t*)arg;
    assert(args != NULL);
    assert(args->ctx != NULL);
    assert(args->rb != NULL);
    assert(args->keep_running != NULL);

    do_rx_stream(args->ctx, args->rb, args->keep_running);
    free(args);
    return NULL;
}

int spawn_rx_thread(rx_ctx_t *ctx, ring_buffer_t *rb, atomic_bool *keep_running, pthread_t *out_thread) {
    assert(ctx != NULL);
    assert(rb != NULL);
    assert(keep_running != NULL);
    assert(out_thread != NULL);
    
    // malloc is needed for the payload to survive the stack cleanup
    rx_thread_args_t *args = malloc(sizeof(rx_thread_args_t));
    if (!args) {
        fprintf(stderr, "ERROR: Failed to allocate memory for rx thread args.\n");
        return EXIT_FAILURE;
    }
    args->ctx = ctx;
    args->rb = rb;
    args->keep_running = keep_running;

    int create_rc = pthread_create(out_thread, NULL, rx_thread_func, args);
    if (create_rc != 0) {
        fprintf(stderr, "ERROR: Failed to spawn RX thread: %s\n", strerror(create_rc));
        free(args);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
