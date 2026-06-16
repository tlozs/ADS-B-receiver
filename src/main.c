#include "rx.h"
#include "ring_buffer.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

// Global shutdown signal flag
volatile sig_atomic_t keep_running = 1;

// Handler function to flip the shutdown signal flag on SIGINT.
// Dummy needed to suppress the "-Wunused-parameter" warning.
void handle_sigint(int dummy) {
    (void)dummy;
    keep_running = 0; 
}

int main() {
    // Catch Ctrl+C, termination requests from systemctl or kill, and terminal closures
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGHUP, handle_sigint);

    // Initialize rx streamer context and the ring buffer
    sdr_ctx_t ctx;
    if (init_usrp(&ctx) != EXIT_SUCCESS) return EXIT_FAILURE;
    ring_buffer_t *rb = ring_buffer_create(ctx.samps_per_buff);

    // Start receiving data
    do_rx_stream(&ctx, rb, &keep_running);

    // 5. This will now safely execute when the loop ends!
    fprintf(stderr, "\nInitiating safe shutdown...\n");
    teardown_usrp(&ctx);
    ring_buffer_destroy(rb);

    return EXIT_SUCCESS;
}