#include "codm_static.hpp"
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {

struct SlotCache {
    uintptr_t slot = 0;
    bool ok = false;
};

static std::unordered_map<uintptr_t, SlotCache> g_slots;
static pid_t g_slotPid = 0;

static bool is_ret(uint32_t i) { return i == 0xD65F03C0u; }

static bool dec_adrp(uint32_t insn, uintptr_t pc, int &rd, uintptr_t &page) {
    if ((insn & 0x9F000000u) != 0x90000000u) return false;
    rd = (int)(insn & 31u);
    const uint64_t immlo = (insn >> 29) & 3u;
    const uint64_t immhi = (insn >> 5) & 0x7FFFFu;
    int64_t imm = (int64_t)((immhi << 2) | immlo);
    imm = (imm << 43) >> 43;
    page = (pc & ~uintptr_t{0xFFF}) + (uintptr_t)(imm << 12);
    return true;
}

static bool dec_add_imm(uint32_t insn, int &rd, int &rn, uint32_t &imm) {
    if ((insn & 0xFFC00000u) != 0x91000000u) return false;
    imm = (insn >> 10) & 0xFFFu;
    rn = (int)((insn >> 5) & 31u);
    rd = (int)(insn & 31u);
    return true;
}

static bool dec_ldr_u64(uint32_t insn, int &rt, int &rn, uint32_t &off) {
    if ((insn & 0xFFC00000u) != 0xF9400000u) return false;
    off = ((insn >> 10) & 0xFFFu) * 8u;
    rn = (int)((insn >> 5) & 31u);
    rt = (int)(insn & 31u);
    return true;
}

/** CBZ/CBNZ, B.cond, HINT — fall through (jnmaini getter null-check path). */
static bool skip_insn(uint32_t in) {
    if ((in & 0xFFFFF01Fu) == 0xD503201Fu) return true;
    const uint32_t top = in >> 24;
    if (top == 0x34 || top == 0x35 || top == 0xB4 || top == 0xB5) return true;
    if (top == 0x36 || top == 0x37 || top == 0xB6 || top == 0xB7) return true;
    if ((in & 0xFF000010u) == 0x54000000u) return true;
    return false;
}

static bool emulate_slot(pid_t pid, uintptr_t fn, uintptr_t &slot, char *err, size_t errCap) {
    uintptr_t reg[32] = {};
    bool known[32] = {};
    slot = 0;
    uintptr_t pc = fn;
    for (int step = 0; step < 48; ++step) {
        uint32_t in = 0;
        if (!mem_read(pid, pc, in)) {
            if (err && errCap) std::snprintf(err, errCap, "read fn fail");
            return false;
        }
        if (is_ret(in)) {
            if (!slot) {
                if (err && errCap) std::snprintf(err, errCap, "no static slot");
                return false;
            }
            return true;
        }
        int rd = 0, rn = 0, rt = 0;
        uintptr_t page = 0;
        uint32_t imm = 0, off = 0;
        if (dec_adrp(in, pc, rd, page)) {
            reg[rd] = page;
            known[rd] = true;
            pc += 4;
            continue;
        }
        if (dec_add_imm(in, rd, rn, imm) && known[rn]) {
            reg[rd] = reg[rn] + imm;
            known[rd] = true;
            pc += 4;
            continue;
        }
        if (dec_ldr_u64(in, rt, rn, off) && known[rn]) {
            const uintptr_t addr = reg[rn] + off;
            uintptr_t val = 0;
            if (!mem_read(pid, addr, val)) {
                known[rt] = false;
                pc += 4;
                continue;
            }
            /* Null is valid (lobby). Keep the slot so the next frame re-reads it. */
            reg[rt] = val;
            known[rt] = true;
            if (rt == 0) slot = addr;
            pc += 4;
            continue;
        }
        /* mov Xd, Xm */
        if ((in & 0xFFE0FFE0u) == 0xAA0003E0u) {
            const int rm = (int)((in >> 16) & 31u);
            const int rdd = (int)(in & 31u);
            if (rm == 31) {
                reg[rdd] = 0;
                known[rdd] = false;
            } else if (known[rm]) {
                reg[rdd] = reg[rm];
                known[rdd] = true;
            }
            pc += 4;
            continue;
        }
        /* B imm26 */
        if ((in & 0xFC000000u) == 0x14000000u) {
            int32_t imm26 = (int32_t)(in & 0x03FFFFFFu);
            imm26 = (imm26 << 6) >> 6;
            pc = pc + (uintptr_t)(int64_t)imm26 * 4u;
            continue;
        }
        if (skip_insn(in)) {
            pc += 4;
            continue;
        }
        /* Unknown op (e.g. 0x08190019). Skip so ADRP/LDR after it still run. */
        pc += 4;
        continue;
    }
    if (err && errCap) std::snprintf(err, errCap, "getter too long");
    return false;
}

} // namespace

bool codm_read_static_ptr(pid_t pid, uintptr_t fn, uintptr_t &out, char *err, size_t errCap) {
    out = 0;
    if (err && errCap) err[0] = '\0';
    if (pid <= 0 || !fn) return false;
    if (g_slotPid != pid) {
        g_slots.clear();
        g_slotPid = pid;
    }
    auto it = g_slots.find(fn);
    if (it == g_slots.end() || !it->second.ok) {
        SlotCache c{};
        char e2[80] = {};
        c.ok = emulate_slot(pid, fn, c.slot, e2, sizeof(e2));
        if (!c.ok || !c.slot) {
            if (err && errCap) std::snprintf(err, errCap, "%s", e2[0] ? e2 : "no slot");
            return false;
        }
        g_slots[fn] = c;
        it = g_slots.find(fn);
    }
    if (!mem_read(pid, it->second.slot, out)) {
        if (err && errCap) std::snprintf(err, errCap, "slot read fail");
        return false;
    }
    return true;
}

void codm_static_reset() { g_slots.clear(); }
