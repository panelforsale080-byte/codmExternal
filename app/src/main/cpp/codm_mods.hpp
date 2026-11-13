#pragma once
#include "codm_engine.hpp"
#include <sys/types.h>
#include <cstdint>

/** libunity hooks.h sites. Does not touch libanogs. */
void codm_mods_apply(pid_t pid, uintptr_t unity, const CodmFeatures &feat, int heldCat);
void codm_mods_reset();
