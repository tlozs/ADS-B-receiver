#pragma once

#include <stdbool.h>
#include <stdint.h>

// Calculates how many longitude zones exist at a given latitude into a lookup table.
void precalculate_nl_table();

// Returns if the decoding was possible or not.
bool decode_global_cpr(int32_t cpr_even_lat, int32_t cpr_even_lon, int32_t cpr_odd_lat, int32_t cpr_odd_lon, bool last_cpr_is_even, double *lat_result, double *lon_result);
