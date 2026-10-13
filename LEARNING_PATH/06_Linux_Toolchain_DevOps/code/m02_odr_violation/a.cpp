// m02_odr_violation/a.cpp
#include "bad_header.h"

#if defined(USE_FIX) && USE_FIX == 1
int g_counter = 0;              // the one and only definition when using fix 1
#endif

void bump() { ++g_counter; }
