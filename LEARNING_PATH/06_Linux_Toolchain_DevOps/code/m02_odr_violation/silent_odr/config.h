// silent_odr/config.h - the SILENT ODR violation.
// Two TUs include this header with different macros defined, so they see two different
// struct layouts under the same name. No linker error. Wrong results or crashes at run time.
#pragma once

struct Config {
#ifdef WITH_DEBUG_FIELDS
    int debug_level;          // present in one TU only: shifts every field after it
#endif
    int port;
    int max_connections;
};

// Defined in writer.cpp (built WITH the extra field), called from reader.cpp (built WITHOUT).
Config make_config();
