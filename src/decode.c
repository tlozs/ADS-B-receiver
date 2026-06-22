#include "decode.h"
#include "ring_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define OVERLAP_SAMPLES 240

static size_t silence_curl_write(void *ptr, size_t size, size_t nmemb, void *stream) {
    (void)ptr; (void)stream;
    return size * nmemb;
}

void convert_sc16_to_u8(const int16_t *restrict src, uint8_t *restrict dest, size_t buff_size) {
    for (size_t i = 0; i < buff_size; ++i) {
        dest[i] = (uint8_t)(((uint16_t)src[i] >> 8) + 128);
    }
}

void init_decode(decode_ctx_t* ctx, size_t samps_per_buff) {
    ctx->samps_per_buff = samps_per_buff;
    ctx->rb = NULL;           // Assigned later dynamically via spawn_decode_thread
    ctx->keep_running = NULL; // Assigned later dynamically via spawn_decode_thread

    ctx->buff_downsampled = malloc(ctx->samps_per_buff * 2 * sizeof(uint8_t));
    ctx->mag = calloc((ctx->samps_per_buff + OVERLAP_SAMPLES), sizeof(uint16_t));

    ctx->mode_s = malloc(sizeof(mode_s_t));
    mode_s_init(ctx->mode_s);
    curl_global_init(CURL_GLOBAL_ALL);
}

static void* decode_thread_entry(void* arg) {
    decode_ctx_t* ctx = (decode_ctx_t*)arg;
    do_decode(ctx);
    return NULL;
}

pthread_t spawn_decode_thread(decode_ctx_t* ctx, ring_buffer_t* rb, volatile sig_atomic_t *keep_running) {
    ctx->rb = rb;
    ctx->keep_running = keep_running;

    pthread_t thread;
    if (pthread_create(&thread, NULL, decode_thread_entry, ctx) != 0) {
        fprintf(stderr, "[ERROR] Failed to spawn decode worker thread\n");
    }
    return thread;
}

void teardown_decode(decode_ctx_t* ctx) {
    if (!ctx) return;

    if (ctx->buff_downsampled) free(ctx->buff_downsampled);
    if (ctx->mag) free(ctx->mag);
    if (ctx->mode_s) free(ctx->mode_s);
}

void on_message(mode_s_t* mode_s, struct mode_s_msg* mm) {
    (void)mode_s; // Suppress unused warning
    // Print only if the checksum is valid
    if (!mm->crcok) {
        return;
    }
    // Extract the 24-bit ICAO address (The unique hardware ID of the plane)
    uint32_t icao = (mm->aa1 << 16) | (mm->aa2 << 8) | mm->aa3;
    printf("=========================================================\n");
    printf("✈  ICAO Address : %06X\n", icao);
        
    if (mm->msgtype == 17) {
        printf("   Message Type : DF17 (Extended Type: %d)\n", mm->metype);
    } else {
        printf("   Message Type : DF%d\n", mm->msgtype);
    }
    // Print the Flight Callsign (e.g., "RYR1234") if available
    // (Only transmitted in specific DF17 messages)
    if (mm->flight[0] != '\0') {
        printf("   Callsign     : %s\n", mm->flight);
    }

    // Print the Altitude
    if (mm->altitude != 0) {
        printf("   Altitude     : %d feet\n", mm->altitude);
    }

    // Print the Velocity and Heading
    if (mm->velocity != 0) {
        printf("   Speed        : %d knots\n", mm->velocity);
        printf("   Heading      : %d degrees\n", (int)mm->heading);
        printf("   Vert. Rate   : %d ft/min\n", mm->vert_rate * 64);
    }

    // Print Raw GPS Coordinates
    // Note: ADS-B uses CPR (Compact Position Reporting).
    // Converting raw lat/lon to real GPS coordinates requires history of previous packets,
    // but we can print the raw encoded values here.
    // Ensure it is a DF17 packet BEFORE checking the metype
    if (mm->msgtype == 17 && mm->metype >= 9 && mm->metype <= 18) {
        printf("   Raw CPR Lat  : %d\n", mm->raw_latitude);
        printf("   Raw CPR Lon  : %d\n", mm->raw_longitude);
    }

    // Print squawk(Identity)
    // Squawk is printed in octal format, hence the %04x (or %04o depending on libmodes' internal maths)
    // Actually, libmodes conveniently converts it to base-10 so we can print it directly as %04d
    if (mm->identity != 0) {
        printf("   Squawk       : %04d\n", mm->identity);
    }
        printf("=========================================================\n\n");

    // INFLUX DB PAYLOAD CONSTRUCTION
    if (mm->altitude == 0) return;
    const char* flight = mm->flight;
    if (flight[0] == '\0') {
        flight = "UNKNOWN";
    }
    // Buffer size for fields
    char payload[512];
    int offset = 0;

    // 1. Build the base tracking structure
    offset += snprintf(payload + offset, sizeof(payload) - offset,
                       "aircraft,icao=%06X,callsign=%s ",
                       icao, flight);

    // 2. Add standard fields (Altitude, Velocity, Heading)
    offset += snprintf(payload + offset, sizeof(payload) - offset,
                       "altitude=%d,velocity=%d,heading=%d",
                       mm->altitude, mm->velocity, (int)mm->heading);

    // 3. Add Squawk field conditionally if it exists
    if (mm->identity != 0) {
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                           ",squawk=%d", mm->identity);
    }

    // 4. Add Raw CPR Location fields conditionally if it is a valid DF17 packet
    if (mm->msgtype == 17 && mm->metype >= 9 && mm->metype <= 18) {
        offset += snprintf(payload + offset, sizeof(payload) - offset,
                           ",cpr_lat=%d,cpr_lon=%d",
                           mm->raw_latitude, mm->raw_longitude);
    }

    //CURL stuff to send the payload to the local InfluxDB instance
    CURL *curl = curl_easy_init();
    if (curl) {
        const char* url = "http://localhost:8086/write?db=adsb";

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, silence_curl_write);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long response_code;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response_code == 204) {
                printf("[LOCAL-DB] %s successfully saved to DB.\n\n", flight);
            } else {
                fprintf(stderr, "error code: %ld\n", response_code);
            }
        } else {
            fprintf(stderr, "failed to connect to server: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }

    // Clear the message state so the next iteration of mode_s_detect 
    // starts with a clean slate, without modifying third-party code.
    memset(mm, 0, sizeof(struct mode_s_msg));
}

// Starts decoding data from the ring buffer until keep_running remains true.
void do_decode(decode_ctx_t* ctx) {
    // Continuously loop as long as the atomic interrupt signal (e.g., Ctrl+C) remains true
    while (*(ctx->keep_running)) {

        // Block/wait until a raw data block is ready in the thread-safe ring buffer, then get its memory pointer
        int16_t* buff = ring_buffer_acquire_read(ctx->rb)->data;

        // Convert raw interleaved SC16 (complex 16-bit IQ data) down into 8-bit unsigned format to save space/cache
        convert_sc16_to_u8(buff, ctx->buff_downsampled, ctx->samps_per_buff * 2);

        // Immediately release our read-lock on the ring buffer block so the SDR thread can reuse it without lag
        ring_buffer_commit_read(ctx->rb);

        // Calculate the signal magnitude vector sqrt(I^2 + Q^2) from raw 8-bit samples, offsetting it past the historical overlap data
        mode_s_compute_magnitude_vector(ctx->buff_downsampled, ctx->mag + OVERLAP_SAMPLES, ctx->samps_per_buff * 2);

        // Scan the newly computed magnitude array to detect valid Mode-S frames and pass found messages to the on_message callback
        mode_s_detect(ctx->mode_s, ctx->mag, ctx->samps_per_buff + OVERLAP_SAMPLES, on_message);

        // Shift the ending 'OVERLAP_SAMPLES' to the very front of the buffer so signals split across blocks aren't lost in the next loop
        memmove(ctx->mag, ctx->mag + ctx->samps_per_buff, OVERLAP_SAMPLES * sizeof(uint16_t));
    }
}
