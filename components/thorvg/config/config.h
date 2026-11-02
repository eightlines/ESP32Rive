// ThorVG build configuration. The upstream build generates this with meson;
// here it is fixed for the ESP32-P4 (and the host test build).
#pragma once

#define THORVG_VERSION_STRING "1.1.3"

#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_PNG_LOADER_SUPPORT 1
#define THORVG_JPG_LOADER_SUPPORT 1
#define THORVG_WEBP_LOADER_SUPPORT 1

// Let ThorVG skip re-rasterising regions that did not change between frames.
#define THORVG_PARTIAL_RENDER_SUPPORT 1

// ThorVG's task scheduler uses std::thread, which ESP-IDF maps onto pthreads /
// FreeRTOS tasks. Off by default; enable with CONFIG_RIVE_THORVG_THREADS.
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if CONFIG_RIVE_THORVG_THREADS
#define THORVG_THREAD_SUPPORT 1
#endif
#elif defined(RIVE_HOST_THORVG_THREADS)
#define THORVG_THREAD_SUPPORT 1
#endif
