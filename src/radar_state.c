#include "radar_state.h"
#include "cpr_math.h"
#include <mode-s.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <string.h>

static uint64_t get_system_tick_ms() {
    struct timespec ts;

    // Try the optimal clock first (Immune to sleep states)
#ifdef CLOCK_BOOTTIME
    if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
        return (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    }
#endif

    // Fallback to standard POSIX monotonic (Fails to track time during sleep)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)(ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
    }

    // The code wont work, kill the process
    fprintf(stderr, "FATAL: Monotonic hardware clocks are completely unavailable.\n");
    exit(EXIT_FAILURE);
}

// Checks if the given squawk code is in a valid octal format (does not contain 8 or 9).
static bool is_valid_squawk(int32_t squawk) {
    if (squawk < 0 || 7777 < squawk) return false;
    int32_t temp = squawk;
    while (temp > 0) {
        if ((temp % 10) > 7) return false;
        temp /= 10;
    }
    return true;
}

// True if the given string only contains uppercase characters or digits or space.
static bool is_valid_callsign(const char *callsign) {
    for (int i = 0; i < 8 && callsign[i] != '\0'; i++) {
        char c = callsign[i];
        if (!isupper(c) && !isdigit(c) && c != ' ')
            return false;
    }
    return true;
}

// A fast, rough approximation for distance in meters between two coordinates.
// Perfect for short distances and low CPU overhead.
static double fast_distance_meters(double lat1, double lon1, double lat2, double lon2) {
    double r_earth = 6371000.0; // Earth radius in meters
    double d_lat = (lat2 - lat1) * (M_PI / 180.0);
    double d_lon = (lon2 - lon1) * (M_PI / 180.0);
    double mean_lat = (lat1 + lat2) / 2.0 * (M_PI / 180.0);
    
    // Equirectangular approximation
    double x = d_lon * cos(mean_lat);
    double y = d_lat;
    
    return sqrt(x * x + y * y) * r_earth;
}

void init_radar_state(radar_state_ctx_t *ctx) {
    if (!ctx) return;
    pthread_mutex_init(&(ctx->mutex), NULL);

    for (size_t i = 0; i < MAX_AIRCRAFT; i++) {
        pthread_mutex_init(&(ctx->repo[i].mutex), NULL);
        ctx->repo[i].icao = 0;
    }

    precalculate_nl_table();
}

void teardown_radar_state(radar_state_ctx_t *ctx) {
    if (!ctx) return;
    pthread_mutex_destroy(&(ctx->mutex));

    for (size_t i = 0; i < MAX_AIRCRAFT; i++)
        pthread_mutex_destroy(&(ctx->repo[i].mutex));
}

aircraft_t *get_or_create_aircraft(radar_state_ctx_t *ctx, uint32_t icao) {
    if (!ctx || icao == 0) return NULL;

    aircraft_t *first_empty_slot = NULL;
    uint64_t now_ms = get_system_tick_ms();

    pthread_mutex_lock(&(ctx->mutex));

    for (size_t i = 0; i < MAX_AIRCRAFT; i++) {
        if (!first_empty_slot && ctx->repo[i].icao == 0)
            first_empty_slot = &(ctx->repo[i]);
        if (ctx->repo[i].icao == icao) {
            pthread_mutex_lock(&(ctx->repo[i].mutex));
            ctx->repo[i].last_update_ms = now_ms;
            pthread_mutex_unlock(&(ctx->repo[i].mutex));
            pthread_mutex_unlock(&(ctx->mutex));        
            return &(ctx->repo[i]);
        }
    }

    if (first_empty_slot) {
        pthread_mutex_lock(&(first_empty_slot->mutex));
        
        // Initialize the fresh state
        first_empty_slot->icao = icao;
        first_empty_slot->is_dirty = false;
        first_empty_slot->landed = false;
        first_empty_slot->last_update_ms = now_ms;
        
        first_empty_slot->cpr_even_time_ms = 0;
        first_empty_slot->cpr_odd_time_ms = 0;
        first_empty_slot->cpr_even_lat = 0;
        first_empty_slot->cpr_even_lon = 0;
        first_empty_slot->cpr_odd_lat = 0;
        first_empty_slot->cpr_odd_lon = 0;

        first_empty_slot->lat = 300.0;
        first_empty_slot->lon = 300.0;
        first_empty_slot->alt_baro = INT32_MIN;
        first_empty_slot->alt_geom = INT32_MIN;
        first_empty_slot->velocity_to_ground = INT32_MIN;
        first_empty_slot->velocity_to_air = INT32_MIN;
        first_empty_slot->heading = INT32_MIN;
        first_empty_slot->vert_rate = INT32_MIN;
        first_empty_slot->squawk = INT32_MIN;
        first_empty_slot->wake_vortex_tc = UINT8_MAX;
        first_empty_slot->wake_vortex_ca = UINT8_MAX;
        first_empty_slot->callsign[0] = '\0';
        
        pthread_mutex_unlock(&(first_empty_slot->mutex));
    }

    pthread_mutex_unlock(&(ctx->mutex));
    return first_empty_slot;
}

aircraft_t *get_aircraft(radar_state_ctx_t *ctx, uint32_t icao) {
    if (!ctx || icao == 0) return NULL;

    uint64_t now_ms = get_system_tick_ms();
    
    pthread_mutex_lock(&(ctx->mutex));

    for (size_t i = 0; i < MAX_AIRCRAFT; i++)
        if (ctx->repo[i].icao == icao) {
            pthread_mutex_lock(&(ctx->repo[i].mutex));
            ctx->repo[i].last_update_ms = now_ms;
            pthread_mutex_unlock(&(ctx->repo[i].mutex));
            pthread_mutex_unlock(&(ctx->mutex));
            return &(ctx->repo[i]);
        }

    pthread_mutex_unlock(&(ctx->mutex));
    return NULL;
}

size_t create_snapshot(radar_state_ctx_t* ctx, aircraft_snapshot_t *snapshot) {
    pthread_mutex_lock(&(ctx->mutex));

    aircraft_t *source = ctx->repo;
    size_t active_count = 0;
    uint64_t now_ms = get_system_tick_ms();

    for (size_t i = 0; i < MAX_AIRCRAFT; i++) {
        if (source[i].icao != 0) {
            aircraft_t *ac = &source[i];
            
            pthread_mutex_lock(&(ac->mutex));
            
            // Only snapshot aircraft that have recent data
            // Also check for uninitialized data for every field
            if (ac->is_dirty) {
                snapshot[active_count].icao = ac->icao;
                snapshot[active_count].lat = ac->lat;
                snapshot[active_count].lon = ac->lon;
                snapshot[active_count].alt_baro = ac->alt_baro;
                snapshot[active_count].alt_geom = ac->alt_geom;
                snapshot[active_count].velocity_to_ground = ac->velocity_to_ground;
                snapshot[active_count].velocity_to_air = ac->velocity_to_air;
                snapshot[active_count].heading = ac->heading;
                snapshot[active_count].vert_rate = ac->vert_rate;
                snapshot[active_count].squawk = ac->squawk;
                snapshot[active_count].wake_vortex_tc = ac->wake_vortex_tc;
                snapshot[active_count].wake_vortex_ca = ac->wake_vortex_ca;
                strncpy(snapshot[active_count].callsign, ac->callsign, 9);
                snapshot[active_count].is_emergency = 
                    now_ms - ac->last_emergency_ms <= STATUS_ALERT_PERSISTENCE;
                snapshot[active_count].is_ident = 
                    now_ms - ac->last_ident_ms <= STATUS_IDENT_PERSISTENCE;
                
                // Mark as clean so we don't send duplicate static data next second
                ac->is_dirty = false;
                active_count++;
            }
            pthread_mutex_unlock(&(ac->mutex));
        }
    }
    pthread_mutex_unlock(&(ctx->mutex));
    return active_count;
}

void update_aircraft_landed(aircraft_t *ac, bool new_status) {
    if (!ac) return;

    pthread_mutex_lock(&(ac->mutex));

    ac->landed = new_status;
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_emergency(aircraft_t *ac) {
    if (!ac) return;

    uint64_t now_ms = get_system_tick_ms();

    pthread_mutex_lock(&(ac->mutex));

    ac->last_emergency_ms = now_ms;
    
    ac->is_dirty = true;
    ac->last_update_ms = now_ms;
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_ident(aircraft_t *ac) {
    if (!ac) return;

    uint64_t now_ms = get_system_tick_ms();

    pthread_mutex_lock(&(ac->mutex));

    ac->last_ident_ms = now_ms;
    
    ac->is_dirty = true;
    ac->last_update_ms = now_ms;
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_flightstatus(aircraft_t *ac, int32_t fs) {
    if (fs == 2 || fs == 3 || fs == 4)
        update_aircraft_emergency(ac);
    if (fs == 4 || fs == 5)
        update_aircraft_ident(ac);
}

void update_aircraft_survstatus(aircraft_t *ac, int32_t ss) {
    if (ss == 1 || ss == 2)
        update_aircraft_emergency(ac);
    if (ss == 3)
        update_aircraft_ident(ac);
}

// TODO: if previous data is available, decode from a single message?
void update_aircraft_coords(aircraft_t *ac, int32_t cpr_lat, int32_t cpr_lon, bool is_even) {
    if (!ac) return;

    uint64_t now_ms = get_system_tick_ms();

    pthread_mutex_lock(&(ac->mutex));
    
    // Update the respective data based on the flag
    if (is_even) {
        ac->cpr_even_lat = cpr_lat;
        ac->cpr_even_lon = cpr_lon;
        ac->cpr_even_time_ms = now_ms;
    }
    else {
        ac->cpr_odd_lat = cpr_lat;
        ac->cpr_odd_lon = cpr_lon;
        ac->cpr_odd_time_ms = now_ms;
    }

    // Explicitly track sequence regardless of timestamp ties
    ac->last_cpr_is_even = is_even;

    // Check if we have enough data to calculate coordinates
    if (ac->cpr_even_time_ms > 0 && ac->cpr_odd_time_ms > 0) {
        
        // Calculate absolute time difference
        uint64_t time_diff = 
            ac->cpr_odd_time_ms < ac->cpr_even_time_ms ? 
            ac->cpr_even_time_ms - ac->cpr_odd_time_ms : 
            ac->cpr_odd_time_ms - ac->cpr_even_time_ms;

        // If packets arrived within 10 seconds, execute the math
        if (time_diff <= 10000) {
            double final_lat = 0.0;
            double final_lon = 0.0;

            bool success = decode_global_cpr(
                ac->cpr_even_lat, ac->cpr_even_lon,
                ac->cpr_odd_lat, ac->cpr_odd_lon, 
                ac->last_cpr_is_even, 
                &final_lat, &final_lon
            );

            if (success) {
                // Validate the decoded data
                if (-90.0 < final_lat && final_lat < 90.0 && 
                    -180.0 < final_lon && final_lon < 180.0 &&
                    !(final_lat == 0.0 && final_lon == 0.0)) {

                    // If previous data is available, perform a 100km distance check
                    if (ac->lat == 300.0 || ac->lon == 300.0 ||
                        fast_distance_meters(ac->lat, ac->lon, final_lat, final_lon) < 100000) {
                        ac->lat = final_lat;
                        ac->lon = final_lon;
                    }
                }
            }
        }
    }
    
    ac->is_dirty = true;
    ac->last_update_ms = now_ms;
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_altitude(aircraft_t *ac, int32_t alt, int32_t unit, bool is_gnss) {
    if (!ac) return;

    if (unit == MODE_S_UNIT_METERS)
        alt = (int)(alt * 3.28084);

    // skip unreasonable altitude levels
    if (alt <= -2000 || 100000 <= alt) return;
        
    pthread_mutex_lock(&(ac->mutex));
    
    if (is_gnss)
        ac->alt_geom = alt;
    else
        ac->alt_baro = alt;
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_velocity(aircraft_t *ac, int32_t velocity, bool is_to_air, int32_t heading, int32_t vert_rate) {
    if (!ac) return;

    // skip impossible kinematics
    if (velocity <= 0 || 3000 <= velocity) return;
    if (heading < 0 || 360 < heading) return;
    if (vert_rate < -25000 || 25000 < vert_rate) return;

    pthread_mutex_lock(&(ac->mutex));
    
    if (is_to_air)
        ac->velocity_to_air = velocity;
    else
        ac->velocity_to_ground = velocity;
    ac->heading = heading;
    ac->vert_rate = vert_rate;
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_squawk(aircraft_t *ac, int32_t squawk) {
    if (!ac || !is_valid_squawk(squawk)) return;

    pthread_mutex_lock(&(ac->mutex));
    
    ac->squawk = squawk;
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_wakevortex(aircraft_t *ac, uint8_t tc, uint8_t ca) {
    if (!ac) return;

    // skip invalid codes
    if (4 < tc || 7 < ca) return;

    pthread_mutex_lock(&(ac->mutex));
    
    ac->wake_vortex_tc = tc;
    ac->wake_vortex_ca = ca;
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

void update_aircraft_callsign(aircraft_t *ac, const char *callsign) {
    if (!ac || !is_valid_callsign(callsign)) return;

    pthread_mutex_lock(&(ac->mutex));
    
    strncpy(ac->callsign, callsign, sizeof(ac->callsign) - 1);
    ac->callsign[sizeof(ac->callsign) - 1] = '\0';
    
    ac->is_dirty = true;
    ac->last_update_ms = get_system_tick_ms();
    
    pthread_mutex_unlock(&(ac->mutex));
}

bool is_aircraft_landed(aircraft_t *ac) {
    if (!ac) return false;
    pthread_mutex_lock(&(ac->mutex));

    bool result = ac->landed;
    
    pthread_mutex_unlock(&(ac->mutex));

    return result;
}
