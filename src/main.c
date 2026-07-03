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

    // Initialize rx streamer context
    rx_ctx_t rx_ctx;
    if (init_usrp(&rx_ctx) != EXIT_SUCCESS) return EXIT_FAILURE;
    
    // Initialize decode context
    decode_ctx_t decode_ctx;
    init_decode(&decode_ctx, rx_ctx.samps_per_buff);
    
    // Initialize ring buffer
    ring_buffer_t *rb = ring_buffer_create(rx_ctx.samps_per_buff);
    if (!rb) {
        fprintf(stderr, "Failed to allocate ring buffer.\n");
        memory_clearup(&decode_ctx, &rx_ctx, NULL, NULL);
        curl_global_cleanup();
        return EXIT_FAILURE;
    }

    // Initialize radar state context
    radar_state_ctx_t radar;
    init_radar_state(&radar);
    g_radar_ctx = &radar;
    
    // Spawn the decode thread
    pthread_t decode_thread = spawn_decode_thread(&decode_ctx, rb, &keep_running);
    if (decode_thread == 0) {
        fprintf(stderr, "Failed to spawn decode thread.\n");
        memory_clearup(&decode_ctx, &rx_ctx, rb, &radar);
        return EXIT_FAILURE;
    }

    // Spawn the receiver thread
    pthread_t rx_thread = spawn_rx_thread(&rx_ctx, rb, &keep_running);
    if (rx_thread == 0) {
        fprintf(stderr, "Failed to spawn rx thread.\n");

        keep_running = 0;
        ring_buffer_abort(rb);
        pthread_join(decode_thread, NULL);

        memory_clearup(&decode_ctx, &rx_ctx, rb, &radar);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\n✈ Radar is LIVE. Listening for ADS-B packets. Press Ctrl+C to stop...\n\n");
    
    run_export_loop(&radar, &keep_running);

    fprintf(stderr, "\nInitiating safe shutdown...\n");

    // Wait for the decode and rx thread to finish its loop
    ring_buffer_abort(rb);
    pthread_join(decode_thread, NULL);
    pthread_join(rx_thread, NULL);

    // Free all allocated memory
    memory_clearup(&decode_ctx, &rx_ctx, rb, &radar);

    return EXIT_SUCCESS;
}
