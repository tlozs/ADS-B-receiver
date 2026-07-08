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

// TODO: include message skipping verbose messages

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

static void print_benchmark_results(time_t start, time_t end) {
    char start_str[32], end_str[32];
        
        // Format timestamps to InfluxDB RFC3339 format (UTC)
    strftime(start_str, sizeof(start_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&start));
    strftime(end_str, sizeof(end_str), "%Y-%m-%dT%H:%M:%SZ", gmtime(&end));

    fprintf(stderr, "\n========================================================================\n");
    fprintf(stderr, "BENCHMARK COMPLETE. RUN THESE QUERIES IN INFLUXDB TO ANALYZE RESULTS:\n");
    fprintf(stderr, "========================================================================\n\n");
    
    fprintf(stderr, "-- 1. Total Unique Aircraft (Capacity Test)\n");
    fprintf(stderr, "SELECT count(DISTINCT(\"icao\")) FROM \"aircraft\" WHERE \"lat\" IS NOT NULL AND time >= '%s' AND time <= '%s'\n\n", start_str, end_str);
    
    fprintf(stderr, "-- 2. Total Telemetry Rows (Reliability Test)\n");
    fprintf(stderr, "SELECT count(\"lat\") FROM \"aircraft\" WHERE time >= '%s' AND time <= '%s'\n\n", start_str, end_str);
    
    fprintf(stderr, "-- 3. Maximum Geographic Range (Horizon Test)\n");
    fprintf(stderr, "SELECT min(\"lat\"), max(\"lat\"), min(\"lon\"), max(\"lon\") FROM \"aircraft\" WHERE time >= '%s' AND time <= '%s'\n\n", start_str, end_str);
    fprintf(stderr, "========================================================================\n\n");
}

int main(int argc, char **argv) {
    bool run_benchmark = false;
    time_t benchmark_start = 0;
    time_t benchmark_end = 0;

    // Check for the benchmark flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--benchmark") == 0) {
            run_benchmark = true;
            break; 
        }
    }


    int exit_status = EXIT_SUCCESS;

    // Register the signals
    if (setup_signals() != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to register signal handlers.\n");
        return EXIT_FAILURE;
    }

    // Curl global state needs to be initialized before any worker thread starts.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        fprintf(stderr, "ERROR: Failed to initialize libcurl.\n");
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
    fprintf(stderr, "Setting up RX context...\n");
    if (init_usrp(&rx_ctx) != EXIT_SUCCESS) {
        exit_status = EXIT_FAILURE;
        goto cleanup_curl;
    }
    fprintf(stderr, "RX context created.\n");
    
    // Initialize decode context
    fprintf(stderr, "Setting up decode context...\n");
    if (init_decode(&decode_ctx, rx_ctx.samps_per_buff) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to initialize decode context.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_rx;
    }
    fprintf(stderr, "Decode context created.\n");
    
    // Initialize ring buffer
    fprintf(stderr, "Creating ring buffer...\n");
    rb = ring_buffer_create(rx_ctx.samps_per_buff);
    if (!rb) {
        fprintf(stderr, "ERROR: Failed to allocate ring buffer.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_decode;
    }
    fprintf(stderr, "Ring buffer created.\n");

    // Initialize radar state
    fprintf(stderr, "Setting up radar...\n");
    if (init_radar_state(&radar) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to initialize radar state.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_rb;
    }
    g_radar_ctx = &radar;
    fprintf(stderr, "Radar created.\n");
    
    // Spawn the decode thread
    fprintf(stderr, "Starting decode thread...\n");
    if (spawn_decode_thread(&decode_ctx, rb, &keep_running, &decode_thread) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to spawn decode thread.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_radar;
    }
    
    // Spawn the receiver thread
    fprintf(stderr, "Starting RX thread...\n");
    if (spawn_rx_thread(&rx_ctx, rb, &keep_running, &rx_thread) != EXIT_SUCCESS) {
        fprintf(stderr, "ERROR: Failed to spawn rx thread.\n");
        atomic_store(&keep_running, false);
        exit_status = EXIT_FAILURE;
        goto cleanup_decode_thread;
    }
    
    fprintf(stderr, "\nRadar is live. Listening for ADS-B packets. Press Ctrl+C to stop...\n\n");
    
    if (run_benchmark) {
        signal(SIGALRM, handle_sigint); 
        alarm(300); 
        benchmark_start = time(NULL);
        fprintf(stderr, "\n>>> Benchmark mode enabled: Daemon will auto-terminate in 5 minutes. <<<\n\n");
    }

    // Start exporting to the database
    if (run_export_loop(&radar, &keep_running) != EXIT_SUCCESS) {
        atomic_store(&keep_running, false);
        exit_status = EXIT_FAILURE;
    }

    if (run_benchmark) benchmark_end = time(NULL);

    fprintf(stderr, "\nInitiating safe shutdown...\n");

    if (run_benchmark && exit_status == EXIT_SUCCESS && benchmark_start != 0 && benchmark_end != 0)
        print_benchmark_results(benchmark_start, benchmark_end);

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
