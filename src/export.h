#pragma once

#include "radar_state.h"
#include <stdatomic.h>

// Exports the contents of the RAM aircraft repository every second.
// Returns EXIT_SUCCESS on a clean shutdown and EXIT_FAILURE on initialization or timing errors.
int run_export_loop(radar_state_ctx_t *ctx, atomic_bool *keep_running);