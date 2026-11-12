#include "codm_anogs.hpp"
#include "codm_offsets.hpp"
#include "codm_proc.hpp"
#include "mem.hpp"
#include "ui_log.hpp"
#include <cerrno>
#include <cstring>

namespace {

/** jnmaini hooks.h PATCH — mov x0, #0; ret */
static const uint32_t kArmPatch[2] = {0xD2800000u, 0xD65F03C0u};

static pid_t s_patchedPid = 0;
static int s_failLogThrottle = 0;

static bool patch_bytes_match(pid_t pid, uintptr_t addr) {
    uint32_t got[2] = {};
    if (!mem_read(pid, addr, got, sizeof(got))) return false;
    return got[0] == kArmPatch[0] && got[1] == kArmPatch[1];
}

static bool apply_one(pid_t pid, uintptr_t addr, char *err, size_t errCap) {
    if (!mem_write_exec(pid, addr, kArmPatch, sizeof(kArmPatch))) {
        if (err && errCap)
            std::snprintf(err, errCap, "write fail 0x%llx", (unsigned long long)addr);
        return false;
    }
    mem_invalidate_maps_cache(pid);
    if (!patch_bytes_match(pid, addr)) {
        if (err && errCap)
            std::snprintf(err, errCap, "verify fail 0x%llx", (unsigned long long)addr);
        return false;
    }
    return true;
}

} // namespace

bool codm_anogs_is_patched(pid_t pid) {
    if (pid <= 0 || s_patchedPid != pid) return false;
    uintptr_t base = 0;
    if (!mem_module_base(pid, "libanogs.so", base)) return false;
    for (size_t i = 0; i < CodmOff::kAnogsPatchCount; ++i) {
        if (!patch_bytes_match(pid, base + CodmOff::kAnogsPatch[i])) return false;
    }
    return true;
}

bool codm_anogs_auto_patch(pid_t pid, uintptr_t anogsBase, char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (pid <= 0 || !anogsBase) {
        if (err && errCap) std::snprintf(err, errCap, "no pid/base");
        return false;
    }
    if (s_patchedPid == pid && codm_anogs_is_patched(pid)) return true;
    if (s_patchedPid != pid) s_patchedPid = 0;

    ui_log("[anogs] pid=%d base=0x%llx — hooks.h PATCH", (int)pid, (unsigned long long)anogsBase);

    for (size_t i = 0; i < CodmOff::kAnogsPatchCount; ++i) {
        const uintptr_t addr = anogsBase + CodmOff::kAnogsPatch[i];
        ui_log("[anogs] site %zu RVA 0x%llx", i + 1, (unsigned long long)CodmOff::kAnogsPatch[i]);
        if (!apply_one(pid, addr, err, errCap)) {
            if (s_failLogThrottle++ % 8 == 0)
                ui_log("[anogs] FAIL %s (need root/adb or in-game ptrace allow)", err ? err : "?");
            s_patchedPid = 0;
            return false;
        }
    }
    s_patchedPid = pid;
    s_failLogThrottle = 0;
    if (err && errCap) std::snprintf(err, errCap, "patched %zu sites", CodmOff::kAnogsPatchCount);
    return true;
}

void codm_anogs_reset_patch_cache() { s_patchedPid = 0; }

void codm_anogs_force_repatch() { s_patchedPid = 0; }
