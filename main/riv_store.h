// A .riv file kept in the "storage" flash partition (8 MB), so an uploaded
// file plays again after a reset without reflashing the app.
//
// Layout: a 64-byte header (magic, size, CRC-32, name) at offset 0, the file
// at offset 4096. The header is written last, so a cut-off upload leaves no
// valid file behind.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"

// Largest file that fits (the partition minus the header sector).
size_t riv_store_capacity();

// Reads the stored file into a new PSRAM buffer (free with heap_caps_free).
// Returns false if there is none or it fails its CRC check.
bool riv_store_load(uint8_t** data, size_t* size, std::string* name);

// Replaces the stored file. Erasing takes ~1 s per MB.
esp_err_t riv_store_save(const uint8_t* data, size_t size, const char* name);

// Removes the stored file (the embedded one plays after the next reset).
esp_err_t riv_store_clear();
