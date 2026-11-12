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

static bool emulate_slot(pid_t pid, uintptr_t fn, uintptr_t &slot, char *err, size_t errCap) {
    uint32_t ins[16] = {};
    if (!mem_read(pid, fn, ins, sizeof(ins))) {
        if (err && errCap) std::snprintf(err, errCap, "read fn fail");
        return false;
    }
    uintptr_t reg[32] = {};
    bool known[32] = {};
    slot = 0;
    for (int i = 0; i < 16; ++i) {
        const uint32_t in = ins[i];
        const uintptr_t pc = fn + (uintptr_t)i * 4u;
        if (is_ret(in)) break;
        int rd = 0, rn = 0, rt = 0;
        uintptr_t page = 0;
        uint32_t imm = 0, off = 0;
        if (dec_adrp(in, pc, rd, page)) {
            reg[rd] = page;
            known[rd] = true;
            continue;
        }
        if (dec_add_imm(in, rd, rn, imm) && known[rn]) {
            reg[rd] = reg[rn] + imm;
            known[rd] = true;
            continue;
        }
        if (dec_ldr_u64(in, rt, rn, off) && known[rn]) {
            const uintptr_t addr = reg[rn] + off;
            uintptr_t val = 0;
            if (!mem_read(pid, addr, val) || !val) {
                if (err && errCap) std::snprintf(err, errCap, "ldr empty 0x%llx", (unsigned long long)addr);
                return false;
            }
            reg[rt] = val;
            known[rt] = true;
            slot = addr;
            continue;
        }
        if (err && errCap) std::snprintf(err, errCap, "insn 0x%08x", in);
        return false;
    }
    if (!slot || !known[0]) {
        if (err && errCap) std::snprintf(err, errCap, "no static slot");
        return false;
    }
    return true;
}

} // namespace

bool codm_read_static_ptr(pid_t pid, uintptr_t fn, uintptr_t &out, char *err, size_t errCap) {
    out = 0;
    if (err && errCap) err[0] = '\0';
    if (pid <= 0 || !fn) return false;
    auto it = g_slots.find(fn);
    if (it == g_slots.end() || !it->second.ok) {
        SlotCache c{};
        char e2[80] = {};
        c.ok = emulate_slot(pid, fn, c.slot, e2, sizeof(e2));
        if (!c.ok) {
            if (err && errCap) std::snprintf(err, errCap, "%s", e2);
            return false;
        }
        g_slots[fn] = c;
        it = g_slots.find(fn);
    }
    if (!it->second.ok || !it->second.slot) return false;
    return mem_read(pid, it->second.slot, out);
}

void codm_static_reset() { g_slots.clear(); }
