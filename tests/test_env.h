// SPDX-License-Identifier: MIT
//
// Portable environment variables for tests: setenv/unsetenv do not exist on MSVC. Tests must build on Windows; use these
// instead of the POSIX calls (and avoid unistd.h, mkdtemp, fork, getpid, sleep in tests).
#pragma once

#include <cstdlib>

inline void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

inline void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");   // an empty value removes the variable
#else
    unsetenv(name);
#endif
}
