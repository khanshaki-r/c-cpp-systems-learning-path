// m02_odr_violation/bad_header.h - THE BUG: a definition in a header.
// Every translation unit that includes this file DEFINES g_counter. Two TUs -> two strong
// definitions of one symbol -> "multiple definition of `g_counter'" at link time.
#pragma once

#ifndef USE_FIX
int g_counter = 0;              // definition (allocates storage) - wrong in a header
#elif USE_FIX == 1
extern int g_counter;           // fix 1: declaration only; a.cpp holds the single definition
#else
inline int g_counter = 0;       // fix 2 (C++17): inline variable, linker merges the copies
#endif

void bump();
int  read_counter();
