#include "codm_engine.hpp"
#include "codm_anogs.hpp"
#include "codm_offsets.hpp"
#include "codm_proc.hpp"
#include "codm_static.hpp"
#include "overlay_jni.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cmath>

static pid_t g_wantPid = 0;
static bool g_anogsToast = false;
static uintptr_t g_matchCache = 0;
static int g_matchLog = 0;

void codm_reset() {
    g_wantPid = 0;
    g_anogsToast = false;
    g_matchCache = 0;
    codm_static_reset();
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
    } else {
        fr.anogsPatched = codm_anogs_is_patched(pid);
        if (!fr.anogsPatched) {
            char perr[96] = {};
            fr.anogsPatched = codm_anogs_auto_patch(pid, fr.anogs, perr, sizeof(perr));
            if (!fr.anogsPatched) {
                std::snprintf(fr.hint, sizeof(fr.hint), "ANOGS patch: %s", perr[0] ? perr : "failed");
            } else if (!g_anogsToast) {
                g_anogsToast = true;
                ui_log("[anogs] Success hook — %s", perr);
                jni_toast("Success hook");
            }
        }
    }

    /* Same as jnmaini GamePlay::get_MatchGame() — read the static the getter loads. */
    uintptr_t match = 0;
    char serr[80] = {};
    const uintptr_t matchFn = fr.unity + CodmOff::kMatchGame;
    const bool gotSlot = codm_read_static_ptr(pid, matchFn, match, serr, sizeof(serr));
    if (!gotSlot || !mem_ptr_ok(match)) {
        if (mem_ptr_ok(g_matchCache) && gotSlot) {
            match = g_matchCache;
        } else if (!gotSlot && (g_matchLog++ % 15) == 0) {
            uintptr_t via = 0;
            RemoteSession s{};
            char rerr[96] = {};
            if (remote_session_begin_try(pid, s)) {
                if (!remote_session_call(s, matchFn, {}, via))
                    std::snprintf(rerr, sizeof(rerr), "%s", s.err);
                remote_session_end(s);
            } else {
                std::snprintf(rerr, sizeof(rerr), "%s", s.err[0] ? s.err : "ptrace busy");
            }
            if (mem_ptr_ok(via)) {
                match = via;
            } else {
                fr.status = CodmStatus::NoMatch;
                std::snprintf(fr.hint, sizeof(fr.hint), "Not in match (%s)",
                              serr[0] ? serr : (rerr[0] ? rerr : "null MatchGame"));
                ui_log("[match] fn=0x%llx %s", (unsigned long long)matchFn, fr.hint);
                return fr;
            }
        } else if (!mem_ptr_ok(match)) {
            fr.status = CodmStatus::NoMatch;
            g_matchCache = 0;
            std::snprintf(fr.hint, sizeof(fr.hint), "Not in match");
            return fr;
        }
    }
    if (!mem_ptr_ok(match)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "Not in match");
        return fr;
    }
    g_matchCache = match;

    uintptr_t enemyList = 0;
    if (!mem_read(pid, match + CodmOff::kEnemyPawns, enemyList) || !mem_ptr_ok(enemyList)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "In lobby (no enemy list)");
        g_matchCache = 0;
        return fr;
    }

    int listSize = 0;
    mem_read(pid, enemyList + CodmOff::kIl2ListSize, listSize);
    std::vector<uintptr_t> pawns;
    int n = 0;
    if (listSize <= 0) {
        fr.status = CodmStatus::Ready;
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — 0 enemies");
        return fr;
    }
    if (!read_il2_list(pid, enemyList, pawns, n)) {
        fr.status = CodmStatus::NoMatch;
        std::snprintf(fr.hint, sizeof(fr.hint), "Enemy list unreadable (size=%d)", listSize);
        return fr;
    }

    fr.status = CodmStatus::Ready;
    std::snprintf(fr.hint, sizeof(fr.hint), "In match — enemies=%d", n);
    if (!feat.esp || n <= 0) return fr;

    RemoteSession sess{};
    const bool haveCall = remote_session_begin_try(pid, sess);
    uintptr_t cam = 0;
    if (haveCall) {
        if (!codm_read_static_ptr(pid, fr.unity + CodmOff::kCameraMain, cam, nullptr, 0) ||
            !mem_ptr_ok(cam)) {
            remote_session_call(sess, fr.unity + CodmOff::kCameraMain, {}, cam);
        }
    }

    int drawn = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t pawn = pawns[static_cast<size_t>(i)];
        if (!mem_ptr_ok(pawn)) continue;
        bool alive = true;
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

        Vec3 root{}, head{};
        bool gotPos = false;
        if (haveCall) {
            gotPos = remote_session_call_vec3(sess, fr.unity + CodmOff::kPawnLastPos, pawn, nullptr, root);
            if (!remote_session_call_vec3(sess, fr.unity + CodmOff::kPawnHeadPos, pawn, nullptr, head))
                head = root;
            if (!gotPos || (root.x == 0.f && root.y == 0.f && root.z == 0.f)) {
                uintptr_t mesh = 0;
                if (mem_read(pid, pawn + CodmOff::kPawnMesh, mesh) && mem_ptr_ok(mesh) &&
                    remote_session_call_vec3(sess, fr.unity + CodmOff::kTransformPos, mesh, nullptr, root))
                    gotPos = true;
                if (head.x == 0.f && head.y == 0.f && head.z == 0.f) head = root;
            }
        }
        if (!gotPos || !std::isfinite(root.x)) continue;
        en.world = root;

        if (haveCall && mem_ptr_ok(cam)) {
            Vec3 rootSc{}, headSc{};
            const bool okR =
                remote_session_call_vec3(sess, fr.unity + CodmOff::kWorldToScreen, cam, &root, rootSc);
            const bool okH =
                remote_session_call_vec3(sess, fr.unity + CodmOff::kWorldToScreen, cam, &head, headSc);
            if (okR && okH && rootSc.z > 0.1f && headSc.z > 0.1f) {
                en.screen = rootSc;
                en.headScreen = headSc;
                en.screenOk = true;
            }
        }
        en.dist = std::sqrt(root.x * root.x + root.z * root.z);
        if (en.screenOk) ++drawn;
        fr.enemies.push_back(en);
    }
    if (haveCall) remote_session_end(sess);
    if (!haveCall)
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — %d enemies (ESP call busy)", n);
    else
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — %d enemies, on screen %d", n, drawn);
    return fr;
}
