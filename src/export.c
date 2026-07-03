#include "export.h"
#include "radar_state.h"
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <curl/curl.h>

// TODO: link libcurl when compiling?

// example Line Protocol output:
// aircraft,icao=A1B2C3 lat=48.12345,lon=8.34567,alt_baro=35000i,alt_geom=35200i,velocity_to_ground=450i,velocity_to_air=465i,heading=270i,vert_rate=-1500i,squawk=7700i,wake_vortex_tc=2i,wake_vortex_ca=4i,callsign="RYR123",is_emergency=t,is_ident=t 1719900000
// 257 characters in total

#define INFLUX_URL "http://localhost:8181/api/v3/write_lp?db=radar&precision=second"
// TODO: safely acquire the access token
#define INFLUX_TOKEN ""

// libcurl passes the incoming data here chunk by chunk.
// We must return the exact number of bytes we were given to tell libcurl we successfully processed them.
static size_t suppress_output_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr; // Suppress unused warning
    (void)userdata; // Suppress unused warning
    // size * nmemb is the total number of bytes in this chunk
    return size * nmemb;
}

// Constructs a payload string in Line Protocol format from the snapshot data
// and sends it as a POST request to the URL defined by the curl network handle.
static void post_to_influx(CURL *curl, aircraft_snapshot_t *snapshot, size_t count) {
    if (!curl || !snapshot || count == 0) return;

    // We allow a generous 512 character limit to a row of the Line Protocol
    size_t max_payload_size = count * 512 * sizeof(char);
    char *payload = malloc(max_payload_size);
    if (!payload) {
        fprintf(stderr, "Failed to allocate InfluxDB payload buffer.\n");
        return;
    }
    size_t offset = 0;
    uint64_t current_time_sec = (uint64_t)time(NULL);

    // A string assembler macro that makes sure no buffer overflow happens
    #define APPEND(...) do { \
        int w = snprintf(payload + offset, max_payload_size - offset, __VA_ARGS__); \
        if (w < 0 || (size_t)w >= (max_payload_size - offset)) { \
            fprintf(stderr, "Buffer overflow prevented during Line Protocol formatting.\n"); \
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
        APPEND("aircraft,icao=%06X ", ac->icao);

        bool has_fields = false;

        if (!(ac->lat == 300.0 || ac->lon == 300.0)) {
            APPEND("%slat=%.5f,lon=%.5f", COMMA, ac->lat, ac->lon);
            has_fields = true;
        }
        if (ac->alt_baro != INT32_MIN) {
            APPEND("%salt_baro=%di", COMMA, ac->alt_baro);
            has_fields = true;
        }
        if (ac->alt_geom != INT32_MIN) {
            APPEND("%salt_geom=%di", COMMA, ac->alt_geom);
            has_fields = true;
        }
        if (ac->velocity_to_ground != INT32_MIN) {
            APPEND("%svelocity_to_ground=%di", COMMA, ac->velocity_to_ground);
            has_fields = true;
        }
        if (ac->velocity_to_air != INT32_MIN) {
            APPEND("%svelocity_to_air=%di", COMMA, ac->velocity_to_air);
            has_fields = true;
        }
        if (ac->heading != INT32_MIN) {
            APPEND("%sheading=%di", COMMA, ac->heading);
            has_fields = true;
        }
        if (ac->vert_rate != INT32_MIN) {
            APPEND("%svert_rate=%di", COMMA, ac->vert_rate);
            has_fields = true;
        }
        if (ac->squawk != INT32_MIN) {
            APPEND("%ssquawk=%di", COMMA, ac->squawk);
            has_fields = true;
        }
        if (ac->wake_vortex_tc != UINT8_MAX) {
            APPEND("%swake_vortex_tc=%ui", COMMA, ac->wake_vortex_tc);
            has_fields = true;
        }
        if (ac->wake_vortex_ca != UINT8_MAX) {
            APPEND("%swake_vortex_ca=%ui", COMMA, ac->wake_vortex_ca);
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
        // The timestamp is appended to the end, a newline closes the data point
        APPEND(" %llu\n", (unsigned long long)current_time_sec);
    }

    // Make a POST request with the payload data
    if (curl) {            
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)offset);
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK)
        fprintf(stderr, "InfluxDB export failed: %s\n", curl_easy_strerror(res));
    }
    else
        fprintf(stderr, "Failed POST to InfluxDB, network handle uninitialized.\n");
        
    #undef APPEND
    #undef COMMA

    free(payload);
}

void run_export_loop(radar_state_ctx_t *ctx, sig_atomic_t *keep_running) {
    if (!ctx) {
        fprintf(stderr, "Radar context does not exist, unable to run the export loop!\n");
        return;
    }

    // Initialize the network handle once, before the main loop
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    if (curl) {
        headers = curl_slist_append(headers, INFLUX_TOKEN);
        curl_easy_setopt(curl, CURLOPT_URL, INFLUX_URL);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, suppress_output_callback);
        // Give the network a maximum of 800 milliseconds to complete the POST
        // This guarantees the export loop run never exceeds 1 second
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 800L);
    }
    else {
        fprintf(stderr, "Failed to set up network handle.\n");
        return;
    }

    // Allocate a local buffer to hold the 1Hz snapshot
    aircraft_snapshot_t snapshot_buffer[MAX_AIRCRAFT];

    // We make sure the export runs every second
    // So we establish the baseline time right before entering the loop
    struct timespec next_wakeup;
    clock_gettime(CLOCK_MONOTONIC, &next_wakeup);

    while (*keep_running) {
        // We need to make the next export exactly 1 second later
        next_wakeup.tv_sec += 1;

        size_t active_count = create_snapshot(ctx, snapshot_buffer);
        if (0 < active_count)
            post_to_influx(curl, snapshot_buffer, active_count);

        // Check if we ran over time before sleeping
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        if (current_time.tv_sec > next_wakeup.tv_sec || 
           (current_time.tv_sec == next_wakeup.tv_sec && current_time.tv_nsec > next_wakeup.tv_nsec)) {
            // We missed our target completely! 
            fprintf(stderr, "WARNING: Export loop overran 1-second boundary. Resetting timer.\n");
            
            // Re-establish the baseline so we don't rapid-fire to catch up
            clock_gettime(CLOCK_MONOTONIC, &next_wakeup);
            // Skip the sleep this time and immediately start the next snapshot
            continue;
        }

        // Sleep until the absolute target time
        // The OS automatically subtracts the time taken by the work
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wakeup, NULL);
    }

    // Clean up network handle when shutting down
    if (curl) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
}
