// silent_odr/writer.cpp - compile with -DWITH_DEBUG_FIELDS
#include "config.h"

Config make_config() {
    Config c{};
#ifdef WITH_DEBUG_FIELDS
    c.debug_level = 3;
#endif
    c.port = 7000;
    c.max_connections = 128;
    return c;
}
