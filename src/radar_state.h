#pragma once

#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdalign.h>

// The size of the aircraft repository.
#define MAX_AIRCRAFT 1024

// An aircraft is cleared from the repository, if no update happens to it for this long.
#define AIRCRAFT_TTL 60000

// The duration of an alert status in ms. 
// If the last alert report is older than this threshold, it is no longer exported as an alert.
#define STATUS_ALERT_PERSISTENCE 2500

// The duration of an ident status in ms. 
// If the last ident report is older than this threshold, it is no longer exported as an ident.
#define STATUS_IDENT_PERSISTENCE 18500

// Stores aircraft data accumulated over multiple decoded ADS-B messages. Contains:
// - ICAO address for unique identification.
// - A mutex, flag for tracking state changes and an update timestamp for TTL management.
// - CPR position data decoded from ADS-B, used to calculate real GPS coordinates.
// - Flight status data about emergency and identification, determined by a latching logic at readout.
// - Clean decoded data ready to be sent to the database.
//
// IMPORTANT: Time data is not epoch time, it is provided by the monotonic clock.
// ALIGNMENT: Aligned to 64 bytes to match standard CPU L1 cache lines. 
// This prevents "false sharing" cache contention between the decoding and 
// exporting threads, guaranteeing optimal multicore performance.
typedef struct {
    alignas(64) pthread_mutex_t mutex;

    uint64_t last_update_ms;
    uint64_t cpr_even_time_ms;
    uint64_t cpr_odd_time_ms;
    uint64_t last_emergency_ms;
    uint64_t last_ident_ms;
    double lat, lon;
    
    int32_t cpr_even_lat, cpr_even_lon;
    int32_t cpr_odd_lat, cpr_odd_lon;
    uint32_t icao;
    int32_t alt_baro;
    int32_t alt_geom;
    int32_t velocity_to_ground;
    int32_t velocity_to_air;
    int32_t heading;
    int32_t vert_rate;
    int32_t squawk;

    // TODO: because the struct is 64-byte aligned, we still can inlude data up to 192 byte without making the struct larger
    bool is_dirty;        
    bool landed;
    bool last_cpr_is_even;
    uint8_t wake_vortex_tc;
    uint8_t wake_vortex_ca;
    char callsign[9];
} aircraft_t;

// The stripped-down version of aircraft_t, only containing data to be exported.
typedef struct {
    double lat, lon;
    
    uint32_t icao;
    int32_t alt_baro;
    int32_t alt_geom;
    int32_t velocity_to_ground;
    int32_t velocity_to_air;
    int32_t heading;
    int32_t vert_rate;
    int32_t squawk;

    bool is_emergency;
    bool is_ident;
    uint8_t wake_vortex_tc;
    uint8_t wake_vortex_ca;
    char callsign[9];
} aircraft_snapshot_t;

// Context necessary for managing the RAM repository. 
// An aircraft entry with ICAO = 0 means an empty slot.
typedef struct {
    aircraft_t repo[MAX_AIRCRAFT];
    pthread_mutex_t mutex;
} radar_state_ctx_t;

// The global pointer to the radar state context, needed inside the on_message callback.
extern radar_state_ctx_t *g_radar_ctx;

// Initializes the aircraft repository inside the RAM, ready to get filled.
int init_radar_state(radar_state_ctx_t *ctx);

// Clears up the aircraft repository from memory. 
void teardown_radar_state(radar_state_ctx_t *ctx);

// Returns a pointer to the aircraft data inside the RAM repository.
// If no aircraft is found, then a blank item is returned with specific initial values.
// A NULL pointer is returned if an error occurred or there is no space left in the repository for a new aircraft.
aircraft_t *get_or_create_aircraft(radar_state_ctx_t *ctx, uint32_t icao);

// Returns a pointer to the aircraft data inside the RAM repository.
// If no aircraft is found, NULL is returned.
aircraft_t *get_aircraft(radar_state_ctx_t *ctx, uint32_t icao);

// Creates a snapshot of the global aircraft RAM repository defined in ctx.
// Returns the number of aircrafts saved.
size_t create_snapshot(radar_state_ctx_t *ctx, aircraft_snapshot_t *snapshot);

void update_aircraft_landed(aircraft_t *ac, bool new_status);
// Updates aircraft last emergency timestamp.
void update_aircraft_emergency(aircraft_t *ac);
// Updates aircraft last ident timestamp.
void update_aircraft_ident(aircraft_t *ac);
// Updates aircraft emergency and ident timestamps based on flight status data.
void update_aircraft_flightstatus(aircraft_t *ac, int32_t fs);
// Updates aircraft emergency and ident timestamps based on surveillance status data.
void update_aircraft_survstatus(aircraft_t *ac, int32_t ss);
// Saves CPR position data and calculates the GPS coordinates if enough data is available.
// If the new decoded location is more than 100km away from the old one, the GPS coordinates are not updated.
void update_aircraft_coords(aircraft_t *ac, int32_t cpr_lat, int32_t cpr_lon, bool is_even);
void update_aircraft_altitude(aircraft_t *ac, int32_t alt, int32_t unit, bool is_gnss);
void update_aircraft_velocity(aircraft_t *ac, int32_t velocity, bool is_to_air, int32_t heading, int32_t vert_rate);
void update_aircraft_squawk(aircraft_t *ac, int32_t squawk);
void update_aircraft_wakevortex(aircraft_t *ac, uint8_t tc, uint8_t ca);
void update_aircraft_callsign(aircraft_t *ac, const char *callsign);

bool is_aircraft_landed(aircraft_t *ac);
