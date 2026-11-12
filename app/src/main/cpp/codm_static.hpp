#pragma once
#include "mem.hpp"
#include <sys/types.h>

/** Follow ADRP/ADD/LDR in a libunity getter and read the final static pointer (no ptrace). */
bool codm_read_static_ptr(pid_t pid, uintptr_t fn, uintptr_t &out, char *err, size_t errCap);
void codm_static_reset();
