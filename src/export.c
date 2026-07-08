#include "export.h"
#include "radar_state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <time.h>
#include <curl/curl.h>
#include <inttypes.h>
#include <assert.h>

// example Line Protocol output:
// aircraft,icao=A1B2C3 lat=48.12345,lon=8.34567,alt_baro=35000i,alt_geom=35200i,velocity_to_ground=450i,velocity_to_air=465i,heading=270i,vert_rate=-1500i,squawk=7700i,wake_vortex_tc=2i,wake_vortex_ca=4i,callsign="RYR123",is_emergency=t,is_ident=t 1719900000
// 257 characters in total

#define INFLUX_URL "http://localhost:8181/api/v3/write_lp?db=radar&no_sync=true&precision=second"

// libcurl passes the incoming data here chunk by chunk.
// We must return the exact number of bytes we were given to tell libcurl we successfully processed them.
static size_t suppress_output_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; // Suppress unused warning
    (void)userdata; // Suppress unused warning
    // size * nmemb is the total number of bytes in this chunk
    return size * nmemb;
}

// Securely zeros memory, defeating compiler Dead Store Elimination (DSE).
// Completely portable across all C standards.
static void secure_memzero(void *ptr, size_t len) {
    if (!ptr || len == 0) return;

    // The volatile qualifier stops the optimizer from analyzing the call.
    void *(*volatile volatile_memset)(void *, int, size_t) = memset;
    volatile_memset(ptr, 0, len);
}

// Dynamically creates the Authorization header using the OS environment variable.
// Returns a heap-allocated string that the caller must free.
static char* create_auth_header_from_env(size_t *out_header_len) {
    assert(out_header_len != NULL);

    // Fetch the token from the OS environment block.
    // getenv() returns process-owned storage, so we only read from it.
    const char *raw_token = getenv("INFLUX_TOKEN");
    if (!raw_token) {
        fprintf(stderr, "ERROR: INFLUX_TOKEN environment variable is not set.\n");
        return NULL;
    }
    size_t raw_token_len = strlen(raw_token);

    // Allocate memory for the formatted header string
    const char *prefix = "Authorization: Token ";
    *out_header_len = strlen(prefix) + raw_token_len + 1;
    
    // Assemble the auth header.
    char *auth_header = malloc(*out_header_len);
    if (!auth_header) {
        fprintf(stderr, "ERROR: Failed to allocate memory for the InfluxDB authorization header.\n");
        return NULL;
    }

    snprintf(auth_header, *out_header_len, "%s%s", prefix, raw_token);
    return auth_header;
}

// Constructs a payload string in Line Protocol format from the snapshot data
// and sends it as a POST request to the URL defined by the curl network handle.
static void post_to_influx(CURL *curl, aircraft_snapshot_t *snapshot, size_t count, char *payload, size_t max_payload_size) {
    assert(curl != NULL);
    assert(snapshot != NULL);
    assert(payload != NULL);
    if (count == 0 || max_payload_size == 0) return;

    size_t offset = 0;
    uint64_t current_time_sec = (uint64_t)time(NULL);

    // A string assembler macro that makes sure no buffer overflow happens
    #define APPEND(...) do { \
        int w = snprintf(payload + offset, max_payload_size - offset, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= (max_payload_size - offset)) { \
            fprintf(stderr, "WARNING: Buffer overflow prevented during Line Protocol formatting.\n"); \
            free(payload); \
            return; \
        } \
        offset += w; \
    } while(0)
    
    // Helper macro to insert commas between fields
    #define COMMA (has_fields ? "," : "")

    // Assemble the payload from all the snapshot data
    for (size_t i = 0; i < count; i++) {
        aircraft_snapshot_t *ac = &snapshot[i];

        // Write the measurement name and the tag (ICAO as a 6-character hex string)
        // PRIX32 handles 32-bit unsigned hex formatting
        APPEND("aircraft,icao=%06" PRIX32 " ", ac->icao);

        bool has_fields = false;

        if (!(ac->lat == 300.0 || ac->lon == 300.0)) {
            APPEND("%slat=%.5f,lon=%.5f", COMMA, ac->lat, ac->lon);
            has_fields = true;
        }
        if (ac->alt_baro != INT32_MIN) {
            APPEND("%salt_baro=%" PRId32 "i", COMMA, ac->alt_baro);
            has_fields = true;
        }
        if (ac->alt_geom != INT32_MIN) {
            APPEND("%salt_geom=%" PRId32 "i", COMMA, ac->alt_geom);
            has_fields = true;
        }
        if (ac->velocity_to_ground != INT32_MIN) {
            APPEND("%svelocity_to_ground=%" PRId32 "i", COMMA, ac->velocity_to_ground);
            has_fields = true;
        }
        if (ac->velocity_to_air != INT32_MIN) {
            APPEND("%svelocity_to_air=%" PRId32 "i", COMMA, ac->velocity_to_air);
            has_fields = true;
        }
        if (ac->heading != INT32_MIN) {
            APPEND("%sheading=%" PRId32 "i", COMMA, ac->heading);
            has_fields = true;
        }
        if (ac->vert_rate != INT32_MIN) {
            APPEND("%svert_rate=%" PRId32 "i", COMMA, ac->vert_rate);
            has_fields = true;
        }
        if (ac->squawk != INT32_MIN) {
            APPEND("%ssquawk=%" PRId32 "i", COMMA, ac->squawk);
            has_fields = true;
        }
        if (ac->wake_vortex_tc != UINT8_MAX) {
            APPEND("%swake_vortex_tc=%" PRIu8 "i", COMMA, ac->wake_vortex_tc);
            has_fields = true;
        }
        if (ac->wake_vortex_ca != UINT8_MAX) {
            APPEND("%swake_vortex_ca=%" PRIu8 "i", COMMA, ac->wake_vortex_ca);
            has_fields = true;
        }
        if (ac->callsign[0] != '\0') {
            APPEND("%scallsign=\"%s\"", COMMA, ac->callsign);
            has_fields = true;
        }
        APPEND("%sis_emergency=%c,is_ident=%c", COMMA, 
                ac->is_emergency ? 't' : 'f', 
                ac->is_ident ? 't' : 'f');
        has_fields = true;
        
        // The timestamp is appended to the end, a newline closes the data point.
        // By using PRIu64, you no longer have to cast current_time_sec to (unsigned long long)
        APPEND(" %" PRIu64 "\n", current_time_sec);
    }

    // Make a POST request with the payload data
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)offset);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
        fprintf(stderr, "WARNING: InfluxDB export failed: %s\n", curl_easy_strerror(res));
    
    #undef APPEND
    #undef COMMA
}

int run_export_loop(radar_state_ctx_t *ctx, atomic_bool *keep_running) {
    assert(ctx != NULL);
    assert(keep_running != NULL);
    int exit_status = EXIT_SUCCESS;

    // Initialize the network handle once, before the main loop
    size_t header_len = 0;
    char *auth_header = create_auth_header_from_env(&header_len);
    if (!auth_header) return EXIT_FAILURE;

    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    if (curl) headers = curl_slist_append(headers, auth_header);
    secure_memzero(auth_header, header_len);
    free(auth_header);

    if (!curl) {
        fprintf(stderr, "ERROR: Failed to set up the InfluxDB network handle.\n");
        return EXIT_FAILURE;
    }
    if (!headers) {
        fprintf(stderr, "ERROR: Failed to allocate curl header list.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_curl;
    }

    if (curl_easy_setopt(curl, CURLOPT_URL, INFLUX_URL) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, suppress_output_callback) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 800L) != CURLE_OK) {
        
        fprintf(stderr, "ERROR: Failed to configure the InfluxDB HTTP client.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_curl_list;
    }

    // Allocate a local buffer to hold the 1Hz snapshot
    aircraft_snapshot_t snapshot_buffer[MAX_AIRCRAFT];

    // Allocate a buffer for POST payload construction
    // We allow a generous 512 character limit to a row of the Line Protocol
    size_t max_payload_size = MAX_AIRCRAFT * 512 * sizeof(char);
    char *payload = malloc(max_payload_size);
    if (!payload) {
        fprintf(stderr, "ERROR: Failed to allocate InfluxDB payload buffer.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_curl_list;
    }

    // Run the export once per second.
    // Establish the baseline time immediately before entering the loop.
    struct timespec next_wakeup;
    if (clock_gettime(CLOCK_MONOTONIC, &next_wakeup) != 0) {
        fprintf(stderr, "ERROR: Failed to read the monotonic clock for export scheduling: %s\n", strerror(errno));
        exit_status = EXIT_FAILURE;
        goto cleanup_payload;
    }

    fprintf(stderr, "Export loop started.\n");

    while (atomic_load(keep_running)) {
        // Schedule the next export exactly one second later.
        next_wakeup.tv_sec += 1;

        size_t active_count = create_snapshot(ctx, snapshot_buffer);
        if (0 < active_count)
            post_to_influx(curl, snapshot_buffer, active_count, payload, max_payload_size);

        // Check if we ran over time before sleeping
        struct timespec current_time;
        if (clock_gettime(CLOCK_MONOTONIC, &current_time) != 0) {
            fprintf(stderr, "ERROR: Failed to read the monotonic clock during export scheduling: %s\n", strerror(errno));
            exit_status = EXIT_FAILURE;
            break;
        }
        
        if (current_time.tv_sec > next_wakeup.tv_sec || 
           (current_time.tv_sec == next_wakeup.tv_sec && current_time.tv_nsec > next_wakeup.tv_nsec)) {
            // We missed our target completely! 
            fprintf(stderr, "WARNING: Export loop overran 1-second boundary. Resetting timer.\n");
            
            // Re-establish the baseline so the loop does not try to catch up in a burst.
            if (clock_gettime(CLOCK_MONOTONIC, &next_wakeup) != 0) {
                fprintf(stderr, "ERROR: Failed to reset the export timer after an overrun: %s\n", strerror(errno));
                exit_status = EXIT_FAILURE;
                break;
            }
            // Skip the sleep this time and immediately start the next snapshot
            continue;
        }

        // Sleep until the absolute target time.
        // The OS automatically accounts for the time spent doing the work.
        int sleep_rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);
        if (sleep_rc != 0 && sleep_rc != EINTR) {
            fprintf(stderr, "ERROR: Export loop sleep failed: %s\n", strerror(sleep_rc));
            exit_status = EXIT_FAILURE;
            break;
        }
    }
    
cleanup_payload:
    free(payload);
cleanup_curl_list:
    if (curl) curl_slist_free_all(headers);
cleanup_curl:
    if (curl) curl_easy_cleanup(curl);
    
    return exit_status;
}
