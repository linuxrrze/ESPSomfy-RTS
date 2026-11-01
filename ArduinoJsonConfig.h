#ifndef arduinojsonconfig_h
#define arduinojsonconfig_h

// Turn off ArduinoJson features this firmware does not need to save flash.
// Always include this header instead of <ArduinoJson.h> so every translation
// unit sees the same configuration. ArduinoJson encodes these settings in its
// inline namespace, so a mismatch shows up as a link error.
//
// Unicode escape decoding (ARDUINOJSON_DECODE_UNICODE) is intentionally left
// on. API clients such as Python's json.dumps escape non-ASCII shade names as
// \uXXXX by default.
#define ARDUINOJSON_USE_LONG_LONG 0
#define ARDUINOJSON_USE_DOUBLE 0
#define ARDUINOJSON_ENABLE_STD_STRING 0
#define ARDUINOJSON_ENABLE_STD_STREAM 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <ArduinoJson.h>

#endif
