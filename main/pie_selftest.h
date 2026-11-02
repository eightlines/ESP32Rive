// Boot self-test for the PIE (ESP32-P4 SIMD) gradient span.
#pragma once

// Checks the PIE translucent gradient span against ThorVG's own loop on
// random data, logs the result and cycles per pixel of each version, and
// turns the PIE span on only if it matched exactly. Call once at boot,
// before rendering starts. No-op unless CONFIG_RIVE_PIE_GRADIENT is set.
void pie_selftest_run();
