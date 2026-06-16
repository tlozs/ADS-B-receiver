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

// POSIX threads require a void* function signature.
// This wrapper casts the argument back to our context struct and launches the loop.
void* decode_thread_func(void* arg) {
    decode_ctx_t* ctx = (decode_ctx_t*)arg;
    do_decode(ctx);
    return NULL;
}

int main() {
    // Catch Ctrl+C, termination requests from systemctl or kill, and terminal closures
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGHUP, handle_sigint);

    // Initialize rx streamer context and the ring buffer
    rx_ctx_t rx_ctx;
    if (init_usrp(&rx_ctx) != EXIT_SUCCESS) return EXIT_FAILURE;
    ring_buffer_t *rb = ring_buffer_create(rx_ctx.samps_per_buff);

    // Initialize decode context
    decode_ctx_t decode_ctx;
    init_decode(&decode_ctx, rb, &keep_running);

    // Spawn the decode thread
    pthread_t decode_thread;
    if (pthread_create(&decode_thread, NULL, decode_thread_func, &decode_ctx) != 0) {
        fprintf(stderr, "Failed to spawn decode thread.\n");
        teardown_decode(&decode_ctx);
        teardown_usrp(&rx_ctx);
        ring_buffer_destroy(rb);
        return EXIT_FAILURE;
    }

    // Start receiving data
    do_rx_stream(&rx_ctx, rb, &keep_running);

    fprintf(stderr, "\nInitiating safe shutdown...\n");
    
    // Wait for the decode thread to finish its loop
    pthread_join(decode_thread, NULL);

    // Free all allocated memory
    teardown_decode(&decode_ctx);
    teardown_usrp(&rx_ctx);
    ring_buffer_destroy(rb);

    return EXIT_SUCCESS;
}
