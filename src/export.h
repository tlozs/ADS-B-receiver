#pragma once

#include "radar_state.h"
#include <signal.h>

// Exports the contents of the RAM aircraft repository every second.
void run_export_loop(radar_state_ctx_t *ctx, volatile sig_atomic_t *keep_running);