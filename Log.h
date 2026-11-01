#ifndef log_h
#define log_h
#include <Arduino.h>

// Serial logging.
//
// LOG_* is always compiled in and is meant for errors and the few messages
// that are needed to diagnose a device in the field (boot, file system).
//
// DEBUG_* is compiled out unless ESPSOMFY_DEBUG is defined, e.g. with
//   arduino-cli compile --build-property "compiler.cpp.extra_flags=-DESPSOMFY_DEBUG" ...
// The debug strings alone take ~38KB of flash, which the ESP32 app partition
// (1.25MB with the default OTA partition table) cannot spare. When disabled the
// arguments are still type checked (including printf formats) but never evaluated.
#define LOG_PRINT(...)   Serial.print(__VA_ARGS__)
#define LOG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define LOG_PRINTF(...)  Serial.printf(__VA_ARGS__)

#ifdef ESPSOMFY_DEBUG
#define DEBUG_PRINT(...)   Serial.print(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#define DEBUG_PRINTF(...)  Serial.printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)   do { if(0) Serial.print(__VA_ARGS__); } while(0)
#define DEBUG_PRINTLN(...) do { if(0) Serial.println(__VA_ARGS__); } while(0)
#define DEBUG_PRINTF(...)  do { if(0) Serial.printf(__VA_ARGS__); } while(0)
#endif

#endif
