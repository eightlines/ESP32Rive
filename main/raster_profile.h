// Per-core breakdown of ThorVG raster time (CONFIG_RIVE_RASTER_PROFILE).
#pragma once

// Logs the time per frame spent in each ThorVG raster / preparation function
// on each core (with calls per frame in brackets) since the last report, and
// resets the counters. No-op when profiling is disabled.
void raster_profile_report(const char* tag, int frames);
