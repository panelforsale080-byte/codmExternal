#include "codm_engine.hpp"
#include "codm_anogs.hpp"
#include "codm_offsets.hpp"
#include "codm_proc.hpp"
#include "codm_static.hpp"
#include "overlay_jni.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>

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

static void read_il2_name(pid_t pid, uintptr_t str, char *out, size_t cap) {
    if (!out || cap < 2) return;
    out[0] = '\0';
    if (!mem_ptr_ok(str)) return;
    int32_t len = 0;
    if (!mem_read(pid, str + 0x10, len) || len <= 0 || len > 48) return;
    uint16_t tmp[48];
    if (!mem_read(pid, str + 0x14, tmp, static_cast<size_t>(len) * 2)) return;
    size_t o = 0;
    for (int i = 0; i < len && o + 1 < cap; ++i) {
        const uint16_t c = tmp[i];
        out[o++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    out[o] = '\0';
}

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

    /* jnmaini: MatchGame* GamePlay::get_MatchGame() */
    uintptr_t match = 0;
    char serr[80] = {};
    const uintptr_t matchFn = fr.unity + CodmOff::kMatchGame;
    codm_read_static_ptr(pid, matchFn, match, serr, sizeof(serr));
    auto match_ok = [&](uintptr_t m) {
        if (!mem_ptr_ok(m)) return false;
        uintptr_t klass = 0;
        return mem_read(pid, m, klass) && mem_ptr_ok(klass);
    };
    if (!match_ok(match) && match_ok(g_matchCache)) match = g_matchCache;
    if (!match_ok(match) && (g_matchLog++ % 4) == 0) {
        uintptr_t via = 0;
        char rerr[96] = {};
        RemoteSession s{};
        if (remote_session_begin_try(pid, s)) {
            if (!remote_session_call(s, matchFn, {}, via))
                std::snprintf(rerr, sizeof(rerr), "%s", s.err[0] ? s.err : "call failed");
            remote_session_end(s);
            ui_log("[match] tid=%d ret=0x%llx %s", (int)s.tid, (unsigned long long)via,
                   rerr[0] ? rerr : "ok");
        } else {
            std::snprintf(rerr, sizeof(rerr), "%s", s.err[0] ? s.err : "ptrace busy");
            ui_log("[match] %s", rerr);
        }
        if (match_ok(via)) match = via;
    }
    if (!match_ok(match)) {
        fr.status = CodmStatus::NoMatch;
        g_matchCache = 0;
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

    Vec3 localPos{};
    bool haveLocal = false;
    if (haveCall) {
        uintptr_t local = 0;
        if (remote_session_call(sess, fr.unity + CodmOff::kLocalPawn, {}, local) && mem_ptr_ok(local))
            haveLocal = remote_session_call_vec3(sess, fr.unity + CodmOff::kPawnLastPos, local, nullptr, localPos);
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
        mem_read(pid, pawn + CodmOff::kPawnIsBot, en.bot);

        uintptr_t atk = 0;
        if (mem_read(pid, pawn + CodmOff::kAtkInfo, atk) && mem_ptr_ok(atk)) {
            mem_read(pid, atk + CodmOff::kHealth, en.hp);
            mem_read(pid, atk + CodmOff::kHealthMax, en.hpMax);
            int state = 0;
            if (mem_read(pid, atk + CodmOff::kPlayerState, state))
                en.knocked = state == CodmOff::kKnocked;
        }
        uintptr_t info = 0;
        if (mem_read(pid, pawn + CodmOff::kPawnPlayerInfo, info) && mem_ptr_ok(info)) {
            mem_read(pid, info + CodmOff::kVest, en.armor);
            mem_read(pid, info + CodmOff::kVestMax, en.armorMax);
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
        const float dx = root.x - (haveLocal ? localPos.x : 0.f);
        const float dy = root.y - (haveLocal ? localPos.y : 0.f);
        const float dz = root.z - (haveLocal ? localPos.z : 0.f);
        en.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!en.screenOk) continue;
        if (en.bot) ++fr.bots;
        else ++fr.players;
        if (feat.ignoreKnocked && en.knocked) continue;
        if (haveCall && feat.name) {
            uintptr_t str = 0;
            if (remote_session_call(sess, fr.unity + CodmOff::kPlayerName, {pawn}, str))
                read_il2_name(pid, str, en.name, sizeof(en.name));
        }
        if (haveCall && feat.playerId) {
            uintptr_t seat = 0;
            if (remote_session_call(sess, fr.unity + CodmOff::kTeamSeat, {pawn}, seat) && seat < 1000)
                en.team = static_cast<int>(seat);
        }
        ++drawn;
        fr.enemies.push_back(en);
    }
    if (haveCall && feat.vehicle && mem_ptr_ok(cam)) {
        uintptr_t vlist = 0;
        std::vector<uintptr_t> vehs;
        int vn = 0;
        if (mem_read(pid, match + CodmOff::kVehicles, vlist) && read_il2_list(pid, vlist, vehs, vn)) {
            if (vn > 12) vn = 12;
            for (int v = 0; v < vn; ++v) {
                uintptr_t tr = 0;
                Vec3 pos{}, sc{};
                if (!remote_session_call(sess, fr.unity + CodmOff::kGetTransform, {vehs[static_cast<size_t>(v)]}, tr))
                    continue;
                if (!mem_ptr_ok(tr)) continue;
                if (!remote_session_call_vec3(sess, fr.unity + CodmOff::kTransformPos, tr, nullptr, pos)) continue;
                if (!remote_session_call_vec3(sess, fr.unity + CodmOff::kWorldToScreen, cam, &pos, sc)) continue;
                if (sc.z <= 0.1f) continue;
                CodmEnemy en{};
                en.vehicle = true;
                en.alive = true;
                en.screenOk = true;
                en.screen = sc;
                en.headScreen = sc;
                en.world = pos;
                en.dist = std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
                std::snprintf(en.name, sizeof(en.name), "VEH");
                fr.enemies.push_back(en);
            }
        }
    }
    if (haveCall) remote_session_end(sess);
    if (!haveCall)
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — %d enemies (ESP call busy)", n);
    else
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — %d enemies, on screen %d", n, drawn);
    return fr;
}
