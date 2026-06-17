#include "rx.h"
#include "ring_buffer.h"
#include "decode.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

// Global shutdown signal flag
volatile sig_atomic_t keep_running = 1;

// Handler function to flip the shutdown signal flag on SIGINT.
// Dummy needed to suppress the "-Wunused-parameter" warning.
void handle_sigint(int dummy) {
    (void)dummy;
    keep_running = 0; 
}

void memory_clearup(decode_ctx_t *decode_ctx, rx_ctx_t *rx_ctx, ring_buffer_t *rb) {
    teardown_decode(decode_ctx);
    teardown_usrp(rx_ctx);
    ring_buffer_destroy(rb);
}

int main() {
    // Catch Ctrl+C, termination requests from systemctl or kill, and terminal closures
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGHUP, handle_sigint);

    // Initialize rx streamer context
    rx_ctx_t rx_ctx;
    if (init_usrp(&rx_ctx) != EXIT_SUCCESS) return EXIT_FAILURE;
    
    // Initialize decode context
    decode_ctx_t decode_ctx;
    init_decode(&decode_ctx, rx_ctx.samps_per_buff);
    
    // Initialize ring buffer
    ring_buffer_t* rb = ring_buffer_create(rx_ctx.samps_per_buff);
    
    // Spawn the thread using our clean factory function
    pthread_t decode_thread = spawn_decode_thread(&decode_ctx, rb, &keep_running);
    if (decode_thread == 0) {
        fprintf(stderr, "Failed to spawn decode thread.\n");
        memory_clearup(&decode_ctx, &rx_ctx, rb);
        return EXIT_FAILURE;
    }

    // Spawn the thread using our clean factory function
    pthread_t rx_thread = spawn_rx_thread(&rx_ctx, rb, &keep_running);
    if (rx_thread == 0) {
        fprintf(stderr, "Failed to spawn rx thread.\n");
        memory_clearup(&decode_ctx, &rx_ctx, rb);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\n✈ Radar is LIVE. Listening for ADS-B packets. Press Ctrl+C to stop...\n\n");
    
    // The Idle Loop: Main thread just sleeps and checks the kill switch
    // Future place of database posting
    while (keep_running) {
        usleep(100000); // Sleep for 100ms so we don't burn CPU
    }

    fprintf(stderr, "\nInitiating safe shutdown...\n");

    // Wait for the decode and rx thread to finish its loop
    ring_buffer_abort(rb);
    pthread_join(decode_thread, NULL);
    pthread_join(rx_thread, NULL);

    // Free all allocated memory
    memory_clearup(&decode_ctx, &rx_ctx, rb);

    return EXIT_SUCCESS;
}
