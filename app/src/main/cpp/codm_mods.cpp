#include "codm_mods.hpp"
#include "mem.hpp"
#include <cstring>

namespace {

enum Site : int {
    UAV = 0,
    PARA,
    CROUCH,
    SWIM,
    SHAKE,
    HITBOX,
    FPS,
    RECOIL,
    PUNCH,
    RELOAD0,
    RELOAD1,
    RELOAD2,
    SPREAD,
    SHOT,
    FOV3,
    FOV1,
    SNOW,
    AIM1,
    AIM2,
    TUTORIAL,
    COUNT
};

struct Slot {
    uintptr_t rva = 0;
    uint8_t orig[16]{};
    uint8_t last[16]{};
    bool have = false;
    bool on = false;
    int cool = 0;
};

Slot g_slot[COUNT];
pid_t g_pid = 0;

static void mov_w0(uint8_t *o, uint32_t imm) {
    const uint32_t a = 0x52800000u | ((imm & 0xFFFFu) << 5);
    const uint32_t b = 0xD65F03C0u;
    std::memcpy(o, &a, 4);
    std::memcpy(o + 4, &b, 4);
    const uint32_t n = 0xD503201Fu;
    std::memcpy(o + 8, &n, 4);
    std::memcpy(o + 12, &n, 4);
}

static void just_ret(uint8_t *o) {
    const uint32_t a = 0xD65F03C0u;
    const uint32_t n = 0xD503201Fu;
    std::memcpy(o, &a, 4);
    std::memcpy(o + 4, &n, 4);
    std::memcpy(o + 8, &n, 4);
    std::memcpy(o + 12, &n, 4);
}

static void fmov0(uint8_t *o, bool pair) {
    const uint32_t s0 = 0x1E2703E0u;
    const uint32_t s1 = pair ? 0x1E2703E1u : 0xD503201Fu;
    const uint32_t ret = 0xD65F03C0u;
    const uint32_t nop = 0xD503201Fu;
    std::memcpy(o, &s0, 4);
    std::memcpy(o + 4, &s1, 4);
    std::memcpy(o + 8, &ret, 4);
    std::memcpy(o + 12, &nop, 4);
}

static void fconst(uint8_t *o, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const uint32_t lo = 0x52800000u | ((bits & 0xFFFFu) << 5);
    const uint32_t hi = 0x72A00000u | ((bits >> 16) << 5);
    const uint32_t mv = 0x1E270000u;
    const uint32_t ret = 0xD65F03C0u;
    std::memcpy(o, &lo, 4);
    std::memcpy(o + 4, &hi, 4);
    std::memcpy(o + 8, &mv, 4);
    std::memcpy(o + 12, &ret, 4);
}

static float pick(float global, float cat) { return cat > 0.f ? cat : global; }

static void set_site(int id, uintptr_t rva, const uint8_t *want, bool enable, pid_t pid, uintptr_t unity) {
    Slot &s = g_slot[id];
    s.rva = rva;
    const uintptr_t addr = unity + rva;
    if (!s.have) {
        if (!mem_read(pid, addr, s.orig, 16)) return;
        std::memcpy(s.last, s.orig, 16);
        s.have = true;
        s.on = false;
    }
    const uint8_t *dst = enable ? want : s.orig;
    if (std::memcmp(s.last, dst, 16) == 0) return;
    if (s.cool > 0) {
        --s.cool;
        return;
    }
    if (!mem_write_exec(pid, addr, dst, 16)) {
        s.cool = 45;
        return;
    }
    std::memcpy(s.last, dst, 16);
    s.on = enable;
}

} // namespace

void codm_mods_reset() {
    g_pid = 0;
    for (Slot &s : g_slot) s = Slot{};
}

void codm_mods_apply(pid_t pid, uintptr_t unity, const CodmFeatures &feat, int heldCat) {
    if (pid <= 0 || !unity) return;
    if (g_pid != pid) {
        codm_mods_reset();
        g_pid = pid;
    }
    const bool cat = heldCat >= 0 && heldCat < 7;
    const auto &w = feat.weapon[cat ? heldCat : 0];
    const bool hitbox = feat.hitbox || (cat && w.hitbox);
    const bool shake = feat.noShake || (cat && w.noShake);
    const float recoil = pick(feat.recoil, cat ? w.recoil : 0.f);
    const float reload = pick(feat.reload, cat ? w.reload : 0.f);
    const float spread = pick(feat.spread, cat ? w.spread : 0.f);
    const float assist = pick(feat.aimAssist, cat ? w.assist : 0.f);

    uint8_t buf[16];
    mov_w0(buf, 1);
    set_site(UAV, 0x6AE9A70, buf, feat.maphack, pid, unity);
    set_site(HITBOX, 0xC2BE0C8, buf, hitbox, pid, unity);

    just_ret(buf);
    set_site(PARA, 0x5A88C48, buf, feat.noParachute, pid, unity);
    set_site(CROUCH, 0x66B882C, buf, feat.noCrouch, pid, unity);

    fconst(buf, 3.f);
    set_site(SWIM, 0x5597B14, buf, feat.fastSwim, pid, unity);

    fmov0(buf, false);
    set_site(SHAKE, 0x669780C, buf, shake, pid, unity);
    set_site(RECOIL, 0xD535A6C, buf, recoil > 1.f, pid, unity);
    set_site(SPREAD, 0xD53408C, buf, spread > 1.f, pid, unity);

    fmov0(buf, true);
    set_site(PUNCH, 0xD5359E4, buf, recoil > 1.f, pid, unity);
    set_site(SHOT, 0xC2BDF40, buf, spread > 1.f, pid, unity);

    fconst(buf, 0.01f);
    set_site(RELOAD0, 0xC2B5B58, buf, reload > 1.f, pid, unity);
    set_site(RELOAD1, 0xC2B5D88, buf, reload > 1.f, pid, unity);
    set_site(RELOAD2, 0xC2B5F90, buf, reload > 1.f, pid, unity);

    const uint32_t fps = feat.frame120 ? 120u : 40u;
    mov_w0(buf, fps);
    set_site(FPS, 0xAFAC73C, buf, feat.frame120 || feat.frame40, pid, unity);

    fconst(buf, feat.fovBr > 1.f ? feat.fovBr : 60.f);
    set_site(FOV3, 0xC0B472C, buf, feat.fovBr > 1.f, pid, unity);
    fconst(buf, feat.fovMp > 1.f ? feat.fovMp : 60.f);
    set_site(FOV1, 0xC0B4458, buf, feat.fovMp > 1.f, pid, unity);
    fconst(buf, feat.snow > 1.f ? feat.snow : 10.f);
    set_site(SNOW, 0x53FE7FC, buf, feat.snow > 1.f, pid, unity);

    fconst(buf, assist > 1.f ? assist : 1.f);
    set_site(AIM1, 0x54F0E64, buf, assist > 1.f, pid, unity);
    set_site(AIM2, 0x66BD5E8, buf, assist > 1.f, pid, unity);

    mov_w0(buf, 0);
    set_site(TUTORIAL, 0xCCC3240, buf, feat.skipTutorial, pid, unity);
}
