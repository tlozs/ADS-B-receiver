#include "cpr_math.h"
#include <math.h>
#include <stddef.h>

// source: https://shemesh.larc.nasa.gov/fm/papers/VSTTE2017-draft.pdf

// Precalculated table, the 0 and 1 indexes are never used, 
// they only act as a padding to simplify indexing.
// Simplified formula:
// for (size_t i = 2; i < 60; i++) {
//     nl_table[i] = 180.0 / M_PI * acos(sin(M_PI/60.0)/sin(M_PI/(double)i));
// }
static const double nl_table[60] = {
    0.0,
    0.0, // Index 0 and 1 are unused padding
    87.000000000000000,
    86.535369975121014,
    85.755416209444192,
    84.891661907020875,
    83.991735629805675,
    83.071994447198165,
    82.139569805106106,
    81.198013492719511,
    80.249232132805147,
    79.294282254569310,
    78.333740829227523,
    77.367894613281919,
    76.396843907944728,
    75.420562566533633,
    74.438934157251424,
    73.451774416678717,
    72.458845447289519,
    71.459864730289894,
    70.454510749876079,
    69.442426311440315,
    68.423220220833400,
    67.396467740846759,
    66.361710083826281,
    65.318453096820988,
    64.266165225674513,
    63.204274793819394,
    62.132166592103424,
    61.049177742463641,
    59.954592766940472,
    58.847637761484712,
    57.727473538661279,
    56.593187562059349,
    55.443784444950609,
    54.278174722729190,
    53.095161527960165,
    51.893424691687869,
    50.671501655538464,
    49.427764392557037,
    48.160391280966536,
    46.867332524987688,
    45.546267226602545,
    44.194549514193156,
    42.809140122435672,
    41.386518322602832,
    39.922566843338934,
    38.412418924123045,
    36.850251075935475,
    35.228995977964452,
    33.539934362985463,
    31.772097076811026,
    29.911356857318385,
    27.938987101219183,
    25.829247070588572,
    23.545044865571406,
    21.029394926029372,
    18.186263570714193,
    14.828174368687542,
    10.470471299968795,
};

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
    
    const double N = 131072.0;

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
