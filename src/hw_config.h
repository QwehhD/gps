// Optional hardware fitted to the board, chosen at compile time like
// NAV_SOURCE (nav_source.h). The board currently has only the ESP32-S3 and
// the display.
//
//   ENABLE_COMPASS  1 = a QMC5883L on I2C (SDA 2, SCL 1). It is probed once
//                   at boot; if it does not answer it is marked absent and
//                   never polled, so a missing sensor logs once. Only used
//                   with NAV_SOURCE_BLE: the simulated sources take the
//                   heading from the simulator. The compass only feeds the
//                   old debug screen (heading label, raw X/Y/Z); the
//                   navigation display uses no heading at all.
//
// Change the default below, or override it per build with
// -DENABLE_COMPASS=1 in platformio.ini's build_flags.
#pragma once

#ifndef ENABLE_COMPASS
#define ENABLE_COMPASS 0
#endif

#if ENABLE_COMPASS != 0 && ENABLE_COMPASS != 1
#error "ENABLE_COMPASS must be 0 or 1"
#endif
