#include "cpr_math.h"
#include <math.h>
#include <stddef.h>

// source: https://shemesh.larc.nasa.gov/fm/papers/VSTTE2017-draft.pdf

// Precalculated table, the 0 and 1 indexes are never used, 
// they only act as a padding to simplify indexing.
double nl_table[60];

void precalculate_nl_table() {
    for (size_t i = 2; i < 60; i++)
        nl_table[i] = 180.0 / M_PI * acos(sin(M_PI/60.0)/sin(M_PI/(double)i));
}

// Looks up the number of longitude zones at a given latitude.
static int get_nl(double lat) {
    double lat_abs = fabs(lat);
    for (size_t i = 2; i < 60; i++)
        if (nl_table[i] < lat_abs)
            return i-1;
    return 59;
}

// Modulo helper for CPR decoding that always returns a non-negative result.
static inline int cpr_mod(int x, int y) {
    int r = x % y;
    return r < 0 ? r + y : r;
}

bool decode_global_cpr(int32_t cpr_even_lat, int32_t cpr_even_lon, int32_t cpr_odd_lat, int32_t cpr_odd_lon, bool last_cpr_is_even, double *lat_result, double *lon_result) {
    if (!lat_result || !lon_result) return false;
    
    double N = 131072.0;

    double dlat_even = 6.0;
    double dlat_odd = 360.0/59.0;
    double c_lat_even = cpr_even_lat / N;
    double c_lat_odd = cpr_odd_lat / N;

    int32_t j = (int32_t) floor(59.0 * c_lat_even - 60.0 * c_lat_odd + 0.5);

    // The recovered latitude coordinates
    double rlat_even = dlat_even * (cpr_mod(j, 60) + c_lat_even);
    double rlat_odd = dlat_odd * (cpr_mod(j, 59) + c_lat_odd);

    // If the two messages fall into different latitude zones, decoding is not possible
    int32_t nl_even = get_nl(rlat_even);
    int32_t nl_odd = get_nl(rlat_odd);
    if (nl_even != nl_odd) return false;

    int32_t n_even = (nl_even < 1 ? 1 : nl_even);
    int32_t n_odd = (nl_odd-1 < 1 ? 1 : nl_odd-1);

    double dlon_even = 360.0 / n_even;
    double dlon_odd = 360.0 / n_odd;
    double c_lon_even = cpr_even_lon / N;
    double c_lon_odd = cpr_odd_lon / N;

    int32_t m = (int32_t) floor((nl_even-1) * c_lon_even - nl_odd * c_lon_odd + 0.5);

    // Return the more recent coordinate pair
    if (last_cpr_is_even) {
        *lat_result = rlat_even;
        *lon_result = dlon_even * (cpr_mod(m, n_even) + c_lon_even);
    }
    else {
        *lat_result = rlat_odd;
        *lon_result = dlon_odd * (cpr_mod(m, n_odd) + c_lon_odd);
    }

    // Normalize latitude into the signed range
    if (*lat_result >= 270.0) 
        *lat_result -= 360.0;
    
    // Normalize longitude into the signed range
    if (*lon_result >= 180.0)
        *lon_result -= 360.0;
    
    return true;
}
