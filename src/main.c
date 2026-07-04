#include "rx.h"
#include "decode.h"
#include "ring_buffer.h"
#include "radar_state.h"
#include "export.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <curl/curl.h>

// TODO: error handling audit, pointer existence checks
// TODO: unit tests?

// TODO: error messages on early return exits?
// TODO: syntax consistency check
// TODO: comment style and wording should be professional

radar_state_ctx_t *g_radar_ctx = NULL;

// Global shutdown signal flag
volatile sig_atomic_t keep_running = 1;

// Handler function to flip the shutdown signal flag on SIGINT.
// Dummy needed to suppress the unused warning.
void handle_sigint(int dummy) {
    (void)dummy; 
    keep_running = 0; 
}

void memory_clearup(decode_ctx_t *decode_ctx, rx_ctx_t *rx_ctx, ring_buffer_t *rb, radar_state_ctx_t *radar) {
    teardown_decode(decode_ctx);
    teardown_usrp(rx_ctx);
    ring_buffer_destroy(rb);
    teardown_radar_state(radar);

    curl_global_cleanup();
}

int main() {
    int exit_status = EXIT_SUCCESS;

    // Catch Ctrl+C, termination requests from systemctl or kill, and terminal closures
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGHUP, handle_sigint);

    // Curl global state needs to be initialized right at the start of the code
    // Because its inherently not thread safe
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl.\n");
        return EXIT_FAILURE;
    }

    // Zero-initialize stack memory so teardown functions don't trip on garbage data
    rx_ctx_t rx_ctx;
    decode_ctx_t decode_ctx;
    radar_state_ctx_t radar;
    ring_buffer_t *rb = NULL;
    pthread_t decode_thread;
    pthread_t rx_thread;
    
    // Initialize rx streamer
    if (init_usrp(&rx_ctx) != EXIT_SUCCESS) {
        exit_status = EXIT_FAILURE;
        goto cleanup_curl;
    }
    
    // Initialize decode context
    if (init_decode(&decode_ctx, rx_ctx.samps_per_buff) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to initialize decode context.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_rx;
    }
    
    // Initialize ring buffer
    rb = ring_buffer_create(rx_ctx.samps_per_buff);
    if (!rb) {
        fprintf(stderr, "Failed to allocate ring buffer.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_decode;
    }

    // Initialize radar state
    if (init_radar_state(&radar) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to initialize radar state.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_rb;
    }
    g_radar_ctx = &radar;
    
    // Spawn the decode thread
    if (spawn_decode_thread(&decode_ctx, rb, &keep_running, &decode_thread) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to spawn decode thread.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_radar;
    }

    // Spawn the receiver thread
    if (spawn_rx_thread(&rx_ctx, rb, &keep_running, &rx_thread) != EXIT_SUCCESS) {
        fprintf(stderr, "Failed to spawn rx thread.\n");
        
        // Safely shut down the decode thread that is already running
        keep_running = 0;
        ring_buffer_abort(rb);
        pthread_join(decode_thread, NULL);
        
        exit_status = EXIT_FAILURE;
        goto cleanup_radar;
    }

    fprintf(stderr, "\n✈ Radar is LIVE. Listening for ADS-B packets. Press Ctrl+C to stop...\n\n");
    
    run_export_loop(&radar, &keep_running);

    fprintf(stderr, "\nInitiating safe shutdown...\n");

    // Normal shutdown path: wait for threads to finish
    ring_buffer_abort(rb);
    pthread_join(decode_thread, NULL);
    pthread_join(rx_thread, NULL);

    // Cascading Cleanup Sequence
cleanup_radar:
    teardown_radar_state(&radar);
cleanup_rb:
    ring_buffer_destroy(rb);
cleanup_decode:
    teardown_decode(&decode_ctx);
cleanup_rx:
    teardown_usrp(&rx_ctx);
cleanup_curl:
    curl_global_cleanup();

    return exit_status;
}
