#ifndef M144_LINEMIC_ROUTE_H_
#define M144_LINEMIC_ROUTE_H_

#include <driver/i2c_master.h>

// M5Stack M144 / Module-Audio STM32 routing helper.
// Register addresses/values from m5stack/M5Module-Audio (I2C 0x33).

bool M144ProbeController(i2c_master_bus_handle_t i2c_bus);
bool M144EnableLineMicPath(i2c_master_bus_handle_t i2c_bus);

#endif  // M144_LINEMIC_ROUTE_H_
