#ifndef M144_ES7210_ISOLATION_H_
#define M144_ES7210_ISOLATION_H_

#include <driver/i2c_master.h>

// CoreS3 onboard ES7210 @ SYS I2C 7-bit 0x40.
// Force SDOUT1 Hi-Z (reg 0x12 SP_SDOUT1_TRI bit4) before EXTERNAL I2S RX
// claims GPIO14. Returns true only when write + readback confirm bit4 set.
bool Es7210ForceSdout1HiZ(i2c_master_bus_handle_t i2c_bus);

#endif  // M144_ES7210_ISOLATION_H_
