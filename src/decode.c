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

void init_decode(decode_ctx_t* ctx, ring_buffer_t* rb, volatile sig_atomic_t *keep_running) {
    ctx->rb = rb;
    ctx->samps_per_buff = rb->samps_per_block;
    ctx->keep_running = keep_running;

    ctx->buff_downsampled = malloc(ctx->samps_per_buff * 2 * sizeof(uint8_t));
    ctx->mag = calloc((ctx->samps_per_buff + OVERLAP_SAMPLES), sizeof(uint16_t));

    ctx->mode_s = malloc(sizeof(mode_s_t));
    mode_s_init(ctx->mode_s);
}

void teardown_decode(decode_ctx_t* ctx) {
    if (!ctx) return;

    if (ctx->buff_downsampled) free(ctx->buff_downsampled);
    if (ctx->mag) free(ctx->mag);
    if (ctx->mode_s) free(ctx->mode_s);
}

void on_message(mode_s_t* mode_s, struct mode_s_msg* mm) {
    (void)mode_s; // Suppress unused warning

    // 1. Drop the packet if the checksum is broken
    if (!mm->crcok) {
        return; 
    }

    // 2. Extract the 24-bit ICAO address (The unique hardware ID of the plane)
    uint32_t icao = (mm->aa1 << 16) | (mm->aa2 << 8) | mm->aa3;

    printf("=========================================================\n");
    printf("✈  ICAO Address : %06X\n", icao);
    printf("   Message Type : DF%d\n", mm->msgtype);

    // 3. Print the Flight Callsign (e.g., "RYR1234") if available
    // (Only transmitted in specific DF17 messages)
    if (mm->flight[0] != '\0') {
        printf("   Callsign     : %s\n", mm->flight);
    }

    // 4. Print the Altitude
    if (mm->altitude != 0) {
        printf("   Altitude     : %d feet\n", mm->altitude);
    }

    // 5. Print the Velocity and Heading
    if (mm->velocity != 0) {
        printf("   Speed        : %d knots\n", mm->velocity);
        printf("   Heading      : %d degrees\n", (int)mm->heading);
        printf("   Vert. Rate   : %d ft/min\n", mm->vert_rate * 64); 
    }

    // 6. Print Raw GPS Coordinates
    // Note: ADS-B uses CPR (Compact Position Reporting). 
    // Converting raw lat/lon to real GPS coordinates requires history of previous packets,
    // but we can print the raw encoded values here.
    if (mm->metype >= 9 && mm->metype <= 18) {
        printf("   Raw CPR Lat  : %d\n", mm->raw_latitude);
        printf("   Raw CPR Lon  : %d\n", mm->raw_longitude);
    }

    printf("=========================================================\n\n");
}

void do_decode(decode_ctx_t* ctx) {

    while (*(ctx->keep_running)) {
        // Acquire read pointer from the ring buffer
        int16_t* buff = ring_buffer_acquire_read(ctx->rb)->data;

        // Downsample sc16 values to u8 expected by the complex->mag conversion
        convert_sc16_to_u8(buff, ctx->buff_downsampled, ctx->samps_per_buff * 2);
        
        // Commit the read to free up buffer space asap
        ring_buffer_commit_read(ctx->rb);

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