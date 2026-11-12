#pragma once
#include "mem.hpp"
#include <sys/types.h>
#include <vector>

bool codm_call_u64(pid_t pid, uintptr_t fn, const std::vector<uintptr_t> &args, uintptr_t &ret);
/** IL2CPP Vector3 often returned through x0 as pointer — read 12 bytes from ret. */
bool codm_call_vec3(pid_t pid, uintptr_t fn, uintptr_t arg0, Vec3 &out);
bool codm_world_to_screen(pid_t pid, uintptr_t fnW2s, uintptr_t camera, const Vec3 &world, Vec3 &screen);
