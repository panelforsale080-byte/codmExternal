#include "codm_engine.hpp"
#include "codm_anogs.hpp"
#include "codm_mods.hpp"
#include "codm_offsets.hpp"
#include "codm_proc.hpp"
#include "codm_static.hpp"
#include "overlay_jni.hpp"
#include "remote_call.hpp"
#include "ui_log.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

static pid_t g_wantPid = 0;
static bool g_anogsToast = false;
static uintptr_t g_matchCache = 0;
static int g_matchLog = 0;
static int g_heldCat = -1;
static int g_wallTick = 0;

void codm_reset() {
    g_wantPid = 0;
    g_anogsToast = false;
    g_matchCache = 0;
    g_heldCat = -1;
    codm_static_reset();
    codm_mods_reset();
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
    codm_mods_apply(pid, fr.unity, feat, g_heldCat);

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
    /* Hijacking UnityMain every frame crashes the match. Try at most once every 2s. */
    static std::chrono::steady_clock::time_point nextCall{};
    const auto now = std::chrono::steady_clock::now();
    if (!match_ok(match) && now >= nextCall) {
        nextCall = now + std::chrono::seconds(2);
        uintptr_t via = 0;
        char rerr[160] = {};
        RemoteSession s{};
        if (remote_session_begin_try(pid, s)) {
            if (!remote_session_call(s, matchFn, {}, via))
                std::snprintf(rerr, sizeof(rerr), "%s", s.err[0] ? s.err : "call failed");
            remote_session_end(s);
            ui_log("[match] tid=%d %s static=%s ret=0x%llx %s", (int)s.tid, s.comm,
                   serr[0] ? serr : "0", (unsigned long long)via, rerr[0] ? rerr : "ok");
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
    const bool wantScan = feat.esp || feat.silentAim || feat.skeleton || feat.vehicle || feat.wallhack;
    if (n <= 0 || !wantScan) {
        fr.status = CodmStatus::Ready;
        std::snprintf(fr.hint, sizeof(fr.hint), "In match — enemies=%d", n);
        return fr;
    }

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
    uintptr_t local = 0;
    bool haveLocal = false;
    if (haveCall) {
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
        float see = 1.f;
        if (mem_read(pid, pawn + CodmOff::kCanSee, see))
            en.visible = see >= -0.01f && see <= 0.01f;

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
        en.headWorld = head;
        if (!en.screenOk) {
            if (feat.silentAim && feat.priority == 0) fr.enemies.push_back(en);
            continue;
        }
        if (en.bot) ++fr.bots;
        else ++fr.players;
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
        if (haveCall && feat.skeleton && drawn < 2 && mem_ptr_ok(cam)) {
            static const uintptr_t kBoneOff[6] = {0x70, 0x90, 0x98, 0x50, 0x68, 0x88};
            uintptr_t map = 0, table = 0;
            if (mem_read(pid, pawn + CodmOff::kBoneMap, map) && mem_ptr_ok(map) &&
                mem_read(pid, map + CodmOff::kBoneTable, table) && mem_ptr_ok(table)) {
                for (int b = 0; b < 6; ++b) {
                    uintptr_t node = 0, tr = 0;
                    Vec3 w{}, sc{};
                    if (!mem_read(pid, table + kBoneOff[b], node) || !mem_ptr_ok(node)) continue;
                    if (!mem_read(pid, node + CodmOff::kBoneNode, tr) || !mem_ptr_ok(tr)) continue;
                    if (!remote_session_call_vec3(sess, fr.unity + CodmOff::kTransformPos, tr, nullptr, w))
                        continue;
                    if (!remote_session_call_vec3(sess, fr.unity + CodmOff::kWorldToScreen, cam, &w, sc))
                        continue;
                    if (sc.z <= 0.05f) continue;
                    en.skel[b] = sc;
                    en.skelOk[b] = true;
                }
            }
        }
        ++drawn;
        fr.enemies.push_back(en);
    }
    if (haveCall && mem_ptr_ok(local)) {
        uintptr_t wpn = 0, cat = 0;
        if (remote_session_call(sess, fr.unity + CodmOff::kCurrentWeapon, {local}, wpn) && mem_ptr_ok(wpn) &&
            remote_session_call(sess, fr.unity + CodmOff::kCategoryName, {wpn, 0}, cat)) {
            switch (static_cast<int>(cat)) {
            case 2: g_heldCat = 1; break;
            case 3: g_heldCat = 2; break;
            case 4: g_heldCat = 3; break;
            case 5: g_heldCat = 4; break;
            case 6: g_heldCat = 5; break;
            case 13: g_heldCat = 6; break;
            default: g_heldCat = 0; break;
            }
        }
        if (feat.wallhack && (g_wallTick++ % 20) == 0) {
            uintptr_t ign = 0;
            remote_session_call(sess, fr.unity + CodmOff::kWallhack, {local, 1}, ign);
        }
        if (feat.silentAim) {
            bool ready = feat.trigger == 0;
            if (!ready) {
                uintptr_t bit = 0;
                if (feat.trigger == 1 || feat.trigger == 3) {
                    if (remote_session_call(sess, fr.unity + CodmOff::kIsFiring, {local}, bit) && bit) ready = true;
                }
                if (!ready && (feat.trigger == 2 || feat.trigger == 3)) {
                    if (remote_session_call(sess, fr.unity + CodmOff::kIsAiming, {local}, bit) && bit) ready = true;
                }
            }
            const CodmEnemy *best = nullptr;
            float bestScore = 1e30f;
            const float cx = feat.screenW * 0.5f;
            const float cy = feat.screenH * 0.5f;
            for (const CodmEnemy &e : fr.enemies) {
                if (!e.alive || e.vehicle) continue;
                if (feat.ignoreKnocked && e.knocked) continue;
                if (!feat.ignoreWall && !feat.disableVis && !e.visible) continue;
                if (feat.aimRange > 1.f && e.dist > feat.aimRange) continue;
                if (feat.priority == 0) {
                    if (e.dist < bestScore) {
                        bestScore = e.dist;
                        best = &e;
                    }
                } else if (e.screenOk) {
                    const float dx = e.headScreen.x - cx;
                    const float dy = (feat.screenH - e.headScreen.y) - cy;
                    const float d2 = dx * dx + dy * dy;
                    if (d2 <= feat.aimFov * feat.aimFov && d2 < bestScore) {
                        bestScore = d2;
                        best = &e;
                    }
                }
            }
            if (ready && best && mem_ptr_ok(cam)) {
                uintptr_t ctr = 0;
                Vec3 camPos{};
                if (remote_session_call(sess, fr.unity + CodmOff::kGetTransform, {cam}, ctr) && mem_ptr_ok(ctr) &&
                    remote_session_call_vec3(sess, fr.unity + CodmOff::kTransformPos, ctr, nullptr, camPos)) {
                    Vec3 aim = best->headWorld;
                    if (feat.targetMode == 0) aim.y = (best->headWorld.y + best->world.y) * 0.5f;
                    else if (feat.targetMode == 2) aim.y -= 0.2f;
                    else if (feat.targetMode == 3) aim = best->world;
                    const float dx = aim.x - camPos.x, dy = aim.y - camPos.y, dz = aim.z - camPos.z;
                    const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (mag > 0.05f) {
                        const Vec3 dir{dx / mag, dy / mag, dz / mag};
                        const Vec3 upv{0.f, 1.f, 0.f};
                        auto cross = [](Vec3 a, Vec3 b) {
                            return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
                        };
                        auto norm = [](Vec3 v) {
                            const float m = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                            if (m < 1e-4f) return Vec3{0, 0, 1};
                            return Vec3{v.x / m, v.y / m, v.z / m};
                        };
                        const Vec3 right = norm(cross(upv, dir));
                        const Vec3 up = cross(dir, right);
                        const float m00 = right.x, m01 = up.x, m02 = dir.x;
                        const float m10 = right.y, m11 = up.y, m12 = dir.y;
                        const float m20 = right.z, m21 = up.z, m22 = dir.z;
                        float qx, qy, qz, qw;
                        const float tr = m00 + m11 + m22;
                        if (tr > 0.f) {
                            const float s = std::sqrt(tr + 1.f) * 2.f;
                            qw = 0.25f * s;
                            qx = (m21 - m12) / s;
                            qy = (m02 - m20) / s;
                            qz = (m10 - m01) / s;
                        } else if (m00 > m11 && m00 > m22) {
                            const float s = std::sqrt(1.f + m00 - m11 - m22) * 2.f;
                            qw = (m21 - m12) / s;
                            qx = 0.25f * s;
                            qy = (m01 + m10) / s;
                            qz = (m02 + m20) / s;
                        } else if (m11 > m22) {
                            const float s = std::sqrt(1.f + m11 - m00 - m22) * 2.f;
                            qw = (m02 - m20) / s;
                            qx = (m01 + m10) / s;
                            qy = 0.25f * s;
                            qz = (m12 + m21) / s;
                        } else {
                            const float s = std::sqrt(1.f + m22 - m00 - m11) * 2.f;
                            qw = (m10 - m01) / s;
                            qx = (m02 + m20) / s;
                            qy = (m12 + m21) / s;
                            qz = 0.25f * s;
                        }
                        float cur[4] = {0, 0, 0, 1};
                        remote_session_call_f(sess, fr.unity + CodmOff::kAimGet, local, nullptr, 0, cur, 4);
                        const float sm = std::clamp(feat.aimSmooth, 1.f, 100.f);
                        const float sp = std::clamp(feat.aimSpeed, 1.f, 100.f);
                        float k = 1.f - std::exp(-((0.25f + (sp / 100.f) * 2.5f) * 0.016f * 60.f));
                        k = k / (1.f + (sm / 100.f) * 3.f);
                        const float cap = 0.005f + (sp / 100.f) * 0.035f;
                        if (k > cap) k = cap;
                        float dot = cur[0] * qx + cur[1] * qy + cur[2] * qz + cur[3] * qw;
                        if (dot < 0.f) {
                            qx = -qx;
                            qy = -qy;
                            qz = -qz;
                            qw = -qw;
                        }
                        float outq[4] = {cur[0] + (qx - cur[0]) * k, cur[1] + (qy - cur[1]) * k,
                                         cur[2] + (qz - cur[2]) * k, cur[3] + (qw - cur[3]) * k};
                        const float qm = std::sqrt(outq[0] * outq[0] + outq[1] * outq[1] + outq[2] * outq[2] +
                                                   outq[3] * outq[3]);
                        if (qm > 1e-4f) {
                            for (float &c : outq) c /= qm;
                            remote_session_call_f(sess, fr.unity + CodmOff::kAimSet, local, outq, 4, nullptr, 0);
                        }
                    }
                }
            }
        }
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
