#include "codm_anogs.hpp"
#include "codm_offsets.hpp"
#include "mem.hpp"
#include <cstring>

namespace {

/** Same as jnmaini ImGui/hooks.h PATCH — mov x0, #0; ret */
static const uint32_t kArmPatch[2] = {0xD2800000u, 0xD65F03C0u};

static pid_t s_patchedPid = 0;

} // namespace

bool codm_anogs_auto_patch(pid_t pid, uintptr_t anogsBase, char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (pid <= 0 || !anogsBase) {
        if (err && errCap) std::snprintf(err, errCap, "no pid/base");
        return false;
    }
    if (s_patchedPid == pid) return true;

    int ok = 0;
    for (size_t i = 0; i < CodmOff::kAnogsPatchCount; ++i) {
        const uintptr_t addr = anogsBase + CodmOff::kAnogsPatch[i];
        if (!mem_write(pid, addr, kArmPatch, sizeof(kArmPatch))) {
            if (err && errCap)
                std::snprintf(err, errCap, "write fail 0x%llx", (unsigned long long)addr);
            return false;
        }
        ++ok;
    }
    s_patchedPid = pid;
    if (err && errCap) std::snprintf(err, errCap, "patched %d sites", ok);
    return ok == (int)CodmOff::kAnogsPatchCount;
}

void codm_anogs_reset_patch_cache() { s_patchedPid = 0; }
