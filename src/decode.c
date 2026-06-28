#include "decode.h"
#include "ring_buffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define OVERLAP_SAMPLES 240

void convert_sc16_to_u8(const int16_t *restrict src, uint8_t *restrict dest, size_t buff_size) {
    for (size_t i = 0; i < buff_size; ++i) {
        dest[i] = (uint8_t)(((uint16_t)src[i] >> 8) + 128);
    }
}

void init_decode(decode_ctx_t *ctx, size_t samps_per_buff) {
    ctx->samps_per_buff = samps_per_buff;

    ctx->buff_downsampled = malloc(ctx->samps_per_buff * 2 * sizeof(uint8_t));
    ctx->mag = calloc((ctx->samps_per_buff + OVERLAP_SAMPLES), sizeof(uint16_t));

    ctx->mode_s = malloc(sizeof(mode_s_t));
    mode_s_init(ctx->mode_s);
}

void teardown_decode(decode_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->buff_downsampled) free(ctx->buff_downsampled);
    if (ctx->mag) free(ctx->mag);
    if (ctx->mode_s) free(ctx->mode_s);
}

void on_message(mode_s_t *mode_s, struct mode_s_msg *mm) {
    (void)mode_s; // Suppress unused warning

    // Only print if the checksum is valid
    if (mm->crcok) {
        
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

        // Print the Squawk Code (Identity)
        // Squawk codes are printed in octal format, hence the %04x (or %04o depending on libmodes' internal math)
        // Actually, libmodes conveniently converts it to base-10 so we can print it directly as %04d
        if (mm->identity != 0) {
            printf("   Squawk       : %04d\n", mm->identity);
        }

        printf("=========================================================\n\n");
    }

    // Clear the message state so the next iteration of mode_s_detect 
    // starts with a clean slate, without modifying third-party code.
    memset(mm, 0, sizeof(struct mode_s_msg));
}

// Starts decoding data from the ring buffer until keep_running remains true.
void do_decode(decode_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {

    while (*keep_running) {
        // Safely acquire the block
        iq_samps_block_t *block = ring_buffer_acquire_read(rb);
        
        // Check for the abort signal to exit the loop so the thread can terminate gracefully
        if (!block) break;

        // Extract the data safely
        int16_t *buff = block->data;

        // Downsample sc16 values to u8 expected by the complex->mag conversion
        convert_sc16_to_u8(buff, ctx->buff_downsampled, ctx->samps_per_buff * 2);
        
        // Commit the read to free up buffer space asap
        ring_buffer_commit_read(rb);

        // Perform the complex->mag conversion
        mode_s_compute_magnitude_vector(ctx->buff_downsampled, ctx->mag + OVERLAP_SAMPLES, ctx->samps_per_buff * 2);

        // Look for an ADS-B message
        mode_s_detect(ctx->mode_s, ctx->mag, ctx->samps_per_buff + OVERLAP_SAMPLES, on_message);

        // Move the overlap tail to the front for the next cycle
        // memmove instead of memcpy is used just in case samps_per_buff 
        // is strangely smaller than 240 and the memory regions overlap
        memmove(ctx->mag, ctx->mag + ctx->samps_per_buff, OVERLAP_SAMPLES * sizeof(uint16_t));
    }
}

// ============================================================================
// Thread Spawning Infrastructure
// ============================================================================

// An ugly payload struct, because pthread_create only accepts void* args
typedef struct {
    decode_ctx_t *ctx;
    ring_buffer_t *rb;
    volatile sig_atomic_t *keep_running;
} decode_thread_args_t;

// The unpacker function to call do_rx_stream with the correct arguments.
// It is explicitly marked 'static' so it is locked to this file.
static void *decode_thread_func(void *arg) {
    decode_thread_args_t *args = (decode_thread_args_t*)arg;
    do_decode(args->ctx, args->rb, args->keep_running);
    free(args);
    return NULL;
}

pthread_t spawn_decode_thread(decode_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {
    // malloc is needed for the payload to survive the stack cleanup
    decode_thread_args_t *args = malloc(sizeof(decode_thread_args_t));
    args->ctx = ctx;
    args->rb = rb;
    args->keep_running = keep_running;

    pthread_t thread;
    if (pthread_create(&thread, NULL, decode_thread_func, args) != 0) {
        fprintf(stderr, "Failed to spawn decode thread.\n");
        free(args);
        // Return a null-equivalent thread ID on failure
        return 0;
    }
    return thread;
}
