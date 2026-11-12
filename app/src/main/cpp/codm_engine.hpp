#pragma once
#include "mem.hpp"
#include <sys/types.h>
#include <vector>

enum class CodmStatus {
    Waiting,
    NoProcess,
    NoUnity,
    NoMatch,
    Ready,
};

struct CodmEnemy {
    uintptr_t pawn = 0;
    Vec3 world{};
    Vec3 screen{};
    float hp = 0;
    float hpMax = 0;
    bool alive = false;
    bool screenOk = false;
};

struct CodmFrame {
    CodmStatus status = CodmStatus::Waiting;
    char hint[96]{};
    pid_t pid = 0;
    uintptr_t unity = 0;
    uintptr_t anogs = 0;
    bool anogsPatched = false;
    std::vector<CodmEnemy> enemies;
};

struct CodmFeatures {
    bool esp = false;
    bool box = true;
    bool line = true;
    bool health = true;
    bool distance = true;
};

void codm_reset();
void codm_set_want_pid(pid_t pid);
pid_t codm_want_pid();
CodmFrame codm_tick(const CodmFeatures &feat);
