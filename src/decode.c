#include "decode.h"
#include "ring_buffer.h"
#include "radar_state.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define OVERLAP_SAMPLES 240

static void convert_sc16_to_u8(const int16_t *restrict src, uint8_t *restrict dest, size_t buff_size) {
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
    ctx->mode_s->check_crc  = 1;
    ctx->mode_s->fix_errors = 1;
    ctx->mode_s->aggressive = 0;
}

void teardown_decode(decode_ctx_t *ctx) {
    if (!ctx) return;

    if (ctx->buff_downsampled) free(ctx->buff_downsampled);
    if (ctx->mag) free(ctx->mag);
    if (ctx->mode_s) free(ctx->mode_s);
}

// Determines if the message is transmitted from an aircraft that is certainly in the air.
// Decides based on the Downlink Format, Flight Status information, Capability code and ADS-B Type Code.
static bool determine_certainly_airborne(struct mode_s_msg *mm) {
    // ACAS replies are only transmitted when airborne
    if(mm->msgtype == 0 || mm->msgtype == 16) return true;

    // Mode S Surveillance replies and Comm-B replies are certainly from airborne, when FS=0 or FS=2
    if(mm->msgtype == 4 || mm->msgtype == 5 || mm->msgtype == 20 || mm->msgtype == 21) 
        if(mm->fs == 0 || mm->fs == 2) return true;

    // Mode S All-call reply is certainly from airborne, if CA=5
    if(mm->msgtype == 11) 
        if(mm->ca == 5) return true;

    // ADS-B is certainly from airborne if CA=5 or it is an Airborne position or airborne velocity message.
    if(mm->msgtype == 17)
        if(mm->ca == 5 || (9 <= mm->metype && mm->metype <= 22)) return true;

    // DF18 actually uses CF in the place of CA with another meaning. 
    // If CF=0 or CF=6, then the message is in standard ADS-B format.
    if(mm->msgtype == 18 && (mm->ca == 0 || mm->ca == 6))
        if (9 <= mm->metype && mm->metype <= 22) return true;

    return false;
}

// Determines if the message is transmitted from an aircraft that is certainly on the ground.
// Decides based on the Downlink Format, Flight Status information, Capability code and ADS-B Type Code.
static bool determine_certainly_on_ground(struct mode_s_msg *mm) {
    // Mode S Surveillance replies and Comm-B replies are certainly from ground, when FS=1 or FS=3
    if(mm->msgtype == 4 || mm->msgtype == 5 || mm->msgtype == 20 || mm->msgtype == 21) 
        if(mm->fs == 1 || mm->fs == 3) return true;

    // Mode S All-call reply is certainly from ground, if CA=4
    if(mm->msgtype == 11) 
        if(mm->ca == 4) return true;

    // ADS-B is certainly from ground if CA=4,
    // or it is an Identification message from a ground vehicle or it is a Surface position message.
    if(mm->msgtype == 17)
        if(mm->ca == 4 || mm->metype == 2 || (5 <= mm->metype && mm->metype <= 8)) return true;

    // DF18 actually uses CF in the place of CA with another meaning. 
    // If CF=0 or CF=6, then the message is in standard ADS-B format.
    if(mm->msgtype == 18 && (mm->ca == 0 || mm->ca == 6))
        if (mm->metype == 2 || (5 <= mm->metype && mm->metype <= 8)) return true;

    return false;
}

// Processes various Mode S messages based on their Downlink Format and Type Code.
// We are interested in DF0,DF4,DF5,DF11,DF17,DF18,DF20,DF21 messages.
// We intend to only track aircraft that is in the air, therefore a filtering is performed.
static void on_message(mode_s_t *mode_s, struct mode_s_msg *mm) {
    (void)mode_s; // Suppress unused warning

    // Only process if radar context is set up
    if (!g_radar_ctx) {
        fprintf(stderr, "Global radar context is not set up, unable to process messages!\n");
        return;
    }

    uint32_t icao = (mm->aa1 << 16) | (mm->aa2 << 8) | mm->aa3;
    aircraft_t *ac = NULL;

    // Here is the only case with an '*_or_create()' call, 
    // so we only register a new aircraft if its certainly in the air.
    if (determine_certainly_airborne(mm)) {
        ac = get_or_create_aircraft(g_radar_ctx, icao);
        // Drop packet if there is no empty space in the registry.
        if (!ac) {
            fprintf(stderr, "RAM aircraft registry full, dropping packet!\n");
            return;
        }
        update_aircraft_landed(ac, false);
    // If the aircraft is certainly from the ground, but it exists in the registry,
    // that means we started tracking it in the air, so
    // we should register that it landed in the meantime (if we didn't already).
    } else if (determine_certainly_on_ground(mm)) {
        ac = get_aircraft(g_radar_ctx, icao);
        if (!ac || is_aircraft_landed(ac)) return;
        update_aircraft_landed(ac, true);
    // Based on the message it cannot be decided whether its coming from an aircraft
    // that is airborne or on the ground. Because we only want to track airborne traffic,
    // the message is only processed, if we already know of the aircraft and its not landed.
    } else {
        ac = get_aircraft(g_radar_ctx, icao);
        if (!ac || is_aircraft_landed(ac)) return;
    }

    // At this point we have a message that is either from an aircraft that:
    // - is certainly airborne
    // - was previously airborne, but it just landed since the last message
    // - has an unconfirmed, either airborne or on ground status, but not yet registered as landed

    switch (mm->msgtype)
    {
    // ACAS replies
    case 0:
    case 16:
        update_aircraft_altitude(ac, mm->altitude, mm->unit, false);
        break;
    // Altitude replies
    case 4:
    case 20:
        update_aircraft_altitude(ac, mm->altitude, mm->unit, false);
        update_aircraft_flightstatus(ac, mm->fs);
        break;
    // Identity replies
    case 5:
    case 21:
        update_aircraft_squawk(ac, mm->identity);
        update_aircraft_flightstatus(ac, mm->fs);
        break;
    // All-call reply
    case 11:
        // The get_or_create_aircraft() call already upadted the timestamp, 
        // there is nothing else to do.
        break;
    // ADS-B and non-transponder ADS-B
    case 17:
    case 18:
        // we cannot process DF18 that is not in an ADS-B format
        if(mm->msgtype == 18 && !(mm->ca == 0 || mm->ca == 6)) break;

        // Aircraft identification
        if (1 <= mm->metype && mm->metype <= 4) {
            update_aircraft_callsign(ac, mm->flight);
            update_aircraft_wakevortex(ac, mm->metype, mm->mesub);
        }
        // Airborne position (w/Baro Altitude)
        else if (9 <= mm->metype && mm->metype <= 18) {
            update_aircraft_altitude(ac, mm->altitude, mm->unit, false);
            update_aircraft_coords(ac, mm->raw_latitude, mm->raw_longitude, !(mm->fflag));
            int ss = (mm->msg[4] & 0x06) >> 1;
            update_aircraft_survstatus(ac, ss);
        }
        // Airborne velocities
        else if (mm->metype == 19) {
            bool is_to_air = mm->mesub == 3 || mm->mesub == 4;
            update_aircraft_velocity(ac, mm->velocity, is_to_air, mm->heading, mm->vert_rate);
        }
        // Airborne position (w/GNSS Height)
        else if (20 <= mm->metype && mm->metype <= 22) {
            update_aircraft_altitude(ac, mm->altitude, mm->unit, true);
            update_aircraft_coords(ac, mm->raw_latitude, mm->raw_longitude, !(mm->fflag));
            int ss = (mm->msg[4] & 0x06) >> 1;
            update_aircraft_survstatus(ac, ss);
        }
        break;
    default:
        break;
    }    
}

// Starts decoding data from the ring buffer until keep_running remains true.
static void do_decode(decode_ctx_t *ctx, ring_buffer_t *rb, volatile sig_atomic_t *keep_running) {

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
