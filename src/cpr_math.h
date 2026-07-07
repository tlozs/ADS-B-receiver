#pragma once

#include <stdbool.h>
#include <stdint.h>

// Returns if the decoding was possible or not.
bool decode_global_cpr(int32_t cpr_even_lat, int32_t cpr_even_lon, int32_t cpr_odd_lat, int32_t cpr_odd_lon, bool last_cpr_is_even, double *lat_result, double *lon_result);
