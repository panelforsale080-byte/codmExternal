#include "codm_engine.hpp"
#include "codm_anogs.hpp"
#include "codm_offsets.hpp"
#include "codm_proc.hpp"
#include "codm_remote.hpp"
#include "overlay_jni.hpp"
#include "ui_log.hpp"
#include <cmath>

static pid_t g_wantPid = 0;
static bool g_anogsToast = false;

void codm_reset() {
    g_wantPid = 0;
    g_anogsToast = false;
    codm_anogs_reset_patch_cache();
}

void codm_set_want_pid(pid_t pid) { g_wantPid = pid; }
pid_t codm_want_pid() { return g_wantPid; }

static bool read_il2_list(pid_t pid, uintptr_t listObj, std::vector<uintptr_t> &ptrs, int &size) {
    ptrs.clear();
    size = 0;
    if (!mem_ptr_ok(listObj)) return false;
    uintptr_t items = 0;
    int n = 0;
    if (!mem_read(pid, listObj + CodmOff::kIl2ListItems, items)) return false;
    if (!mem_read(pid, listObj + CodmOff::kIl2ListSize, n)) return false;
    if (n <= 0 || n > 128 || !mem_ptr_ok(items)) return false;
    size = n;
    ptrs.resize(static_cast<size_t>(n));
    return mem_read(pid, items + CodmOff::kIl2ArrayData, ptrs.data(),
                    ptrs.size() * sizeof(uintptr_t));
}

CodmFrame codm_tick(const CodmFeatures &feat) {
    CodmFrame fr{};
    pid_t pid = g_wantPid;
    if (pid <= 0) pid = proc_best_codm();
    if (pid <= 0) {
        fr.status = CodmStatus::NoProcess;
        std::snprintf(fr.hint, sizeof(fr.hint), "Waiting… open CODM (com.garena.game.codm)");
        return fr;
    }
    g_wantPid = pid;
    fr.pid = pid;
    if (!proc_alive(pid)) {
        fr.status = CodmStatus::NoProcess;
        std::snprintf(fr.hint, sizeof(fr.hint), "Process ended");
        codm_reset();
        return fr;
    }

    if (!mem_module_base(pid, "libunity.so", fr.unity)) {
        fr.status = CodmStatus::NoUnity;
        std::snprintf(fr.hint, sizeof(fr.hint), "No libunity.so");
        return fr;
    }
    if (!mem_module_base(pid, "libanogs.so", fr.anogs)) {
        std::snprintf(fr.hint, sizeof(fr.hint), "No libanogs.so yet");
    } else if (!fr.anogsPatched) {
        char perr[80] = {};
        fr.anogsPatched = codm_anogs_auto_patch(pid, fr.anogs, perr, sizeof(perr));
        if (fr.anogsPatched && !g_anogsToast) {
            g_anogsToast = true;
            ui_log("[anogs] Success hook — %s", perr);
            jni_toast("Success hook");
        }
    }

    uintptr_t match = 0;
    if (!codm_call_u64(pid, fr.unity + CodmOff::kMatchGame, {}, match) || !mem_ptr_ok(match)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "Not in match");
        return fr;
    }

    uintptr_t enemyList = 0;
    if (!mem_read(pid, match + CodmOff::kEnemyPawns, enemyList) || !mem_ptr_ok(enemyList)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "No enemy list");
        return fr;
    }

    std::vector<uintptr_t> pawns;
    int n = 0;
    if (!read_il2_list(pid, enemyList, pawns, n)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "List read fail");
        return fr;
    }

    uintptr_t cam = 0;
    codm_call_u64(pid, fr.unity + CodmOff::kCameraMain, {}, cam);

    fr.status = CodmStatus::Ready;
    std::snprintf(fr.hint, sizeof(fr.hint), "OK enemies=%d", n);

    if (!feat.esp) return fr;

    for (int i = 0; i < n; ++i) {
        const uintptr_t pawn = pawns[static_cast<size_t>(i)];
        if (!mem_ptr_ok(pawn)) continue;
        bool alive = false;
        mem_read(pid, pawn + CodmOff::kPawnIsAlive, alive);
        if (!alive) continue;

        CodmEnemy en{};
        en.pawn = pawn;
        en.alive = true;

        uintptr_t atk = 0;
        if (mem_read(pid, pawn + CodmOff::kAtkInfo, atk) && mem_ptr_ok(atk)) {
            mem_read(pid, atk + CodmOff::kHealth, en.hp);
            mem_read(pid, atk + CodmOff::kHealthMax, en.hpMax);
        }

        Vec3 pos{};
        if (!codm_call_vec3(pid, fr.unity + CodmOff::kPawnLastPos, pawn, pos)) {
            uintptr_t mesh = 0;
            if (mem_read(pid, pawn + CodmOff::kPawnMesh, mesh) && mem_ptr_ok(mesh))
                codm_call_vec3(pid, fr.unity + CodmOff::kTransformPos, mesh, pos);
        }
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) continue;
        en.world = pos;

        if (mem_ptr_ok(cam)) {
            Vec3 scr{};
            if (codm_world_to_screen(pid, fr.unity + CodmOff::kWorldToScreen, cam, pos, scr)) {
                en.screen = scr;
                en.screenOk = scr.z > 0.01f && scr.x >= 0.f && scr.y >= 0.f;
            }
        }
        fr.enemies.push_back(en);
    }
    return fr;
}
