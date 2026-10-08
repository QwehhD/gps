// Compile-time choice of where the navigation display gets its data from.
//
//   NAV_SOURCE_SIM       nav_sim drives the UI directly. No QMC5883L I2C
//                        traffic, no real GPS/BLE data is read.
//   NAV_SOURCE_LOOPBACK  nav_sim output goes through nav_encode() and
//                        nav_decode() before reaching the UI, proving the
//                        BLE wire format without a radio.
//   NAV_SOURCE_BLE       real data from the phone over BLE.
//
// There is deliberately no runtime fallback from BLE to the simulator: a
// made-up route on screen while the link is down is dangerous on the road.
//
// Change the default below, or override it per build with
// -DNAV_SOURCE=NAV_SOURCE_LOOPBACK in platformio.ini's build_flags.
#pragma once

// Start at 1: an undefined or misspelled name evaluates to 0 in #if and is
// caught by the check below instead of silently picking a source.
#define NAV_SOURCE_SIM 1
#define NAV_SOURCE_LOOPBACK 2
#define NAV_SOURCE_BLE 3

#ifndef NAV_SOURCE
#define NAV_SOURCE NAV_SOURCE_SIM
#endif

#if NAV_SOURCE != NAV_SOURCE_SIM && NAV_SOURCE != NAV_SOURCE_LOOPBACK && NAV_SOURCE != NAV_SOURCE_BLE
#error "NAV_SOURCE must be NAV_SOURCE_SIM, NAV_SOURCE_LOOPBACK or NAV_SOURCE_BLE"
#endif
