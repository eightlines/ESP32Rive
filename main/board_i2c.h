// The board's shared I2C bus (SDA GPIO 7, SCL GPIO 8): GT911 touch, ES8311
// audio codec, ES7210 microphone ADC and the camera connector all sit on it.
#pragma once

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates the bus on first use; later calls return the same handle. NULL if
// the bus could not be created.
i2c_master_bus_handle_t board_i2c_bus(void);

#ifdef __cplusplus
}
#endif
