// silent_odr/reader.cpp - compile WITHOUT -DWITH_DEBUG_FIELDS
#include <cstdio>

#include "config.h"

int main() {
    const Config c = make_config();
    // writer.cpp returned {debug_level=3, port=7000, max_connections=128} in a 12-byte struct.
    // This TU believes Config is 8 bytes {port, max_connections} and reads 3 and 7000.
    std::printf("port=%d max_connections=%d\n", c.port, c.max_connections);
    return 0;
}
