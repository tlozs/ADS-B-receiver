#include "rx.h"
#include "decode.h"
#include "ring_buffer.h"
#include "radar_state.h"
#include "export.h"
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <curl/curl.h>

// TODO: error handling audit, pointer existence checks
// TODO: unit tests?
// TODO: error messages on early return exits?
// TODO: syntax consistency check
// TODO: comment style and wording should be professional

// TODO: install instructions for influxdb, 1500 user added

radar_state_ctx_t *g_radar_ctx = NULL;

// Global shutdown signal flag, ensuring multi-thread safety
atomic_bool keep_running = true;

// Signal handler that flips the shutdown flag.
static void handle_sigint(int dummy) {
    (void)dummy; 
    atomic_store(&keep_running, false);
}

// Encapsulates all signal routing setup.
static int setup_signals() {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    
    // Block other signals from interrupting the handler while it runs
    sigemptyset(&(sa.sa_mask));
    
    // SA_RESTART ensures that if a signal interrupts a system call, 
    // the system call transparently resumes instead of failing with EINTR.
    sa.sa_flags = SA_RESTART; 

    if (sigaction(SIGINT, &sa, NULL) != 0 ||
        sigaction(SIGTERM, &sa, NULL) != 0 ||
        sigaction(SIGHUP, &sa, NULL) != 0)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

int main() {
    int exit_status = EXIT_SUCCESS;

    // Register the signals
    if (setup_signals() != EXIT_SUCCESS) {
        fprintf(stderr, "FATAL: Failed to register signal handlers.\n");
        return EXIT_FAILURE;
    }

    // Curl global state needs to be initialized before any worker thread starts.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "Failed to initialize libcurl.\n");
        return EXIT_FAILURE;
    }

    // Zero-initialize stack storage so cleanup helpers can safely run on early failure.
    rx_ctx_t rx_ctx = {0};
    decode_ctx_t decode_ctx = {0};
    radar_state_ctx_t radar = {0};
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
        atomic_store(&keep_running, false);
        exit_status = EXIT_FAILURE;
        goto cleanup_decode_thread;
    }

    fprintf(stderr, "\nRadar is live. Listening for ADS-B packets. Press Ctrl+C to stop...\n\n");
    
    // Start exporting to the database
    if (run_export_loop(&radar, &keep_running) != EXIT_SUCCESS) {
        atomic_store(&keep_running, false);
        exit_status = EXIT_FAILURE;
    }

    fprintf(stderr, "\nInitiating safe shutdown...\n");

    // Normal shutdown path: wait for threads to finish
    int rx_join_rc = pthread_join(rx_thread, NULL);
    if (rx_join_rc != 0)
        fprintf(stderr, "ERROR: Failed to join RX thread during shutdown: %s\n", strerror(rx_join_rc));

    // Cascading cleanup sequence
cleanup_decode_thread:
    ring_buffer_abort(rb);
    int decode_join_rc = pthread_join(decode_thread, NULL);
    if (decode_join_rc != 0)
        fprintf(stderr, "ERROR: Failed to join decode thread during shutdown: %s\n", strerror(decode_join_rc));
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
