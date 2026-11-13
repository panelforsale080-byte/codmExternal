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
    Vec3 headScreen{};
    float dist = 0;
    float hp = 0;
    float hpMax = 0;
    int armor = 0;
    int armorMax = 0;
    bool bot = false;
    bool knocked = false;
    bool alive = false;
    bool screenOk = false;
    bool vehicle = false;
    int team = -1;
    char name[32]{};
};

struct CodmFrame {
    CodmStatus status = CodmStatus::Waiting;
    char hint[96]{};
    pid_t pid = 0;
    uintptr_t unity = 0;
    uintptr_t anogs = 0;
    bool anogsPatched = false;
    int players = 0;
    int bots = 0;
    std::vector<CodmEnemy> enemies;
};

struct CodmFeatures {
    bool esp = false;
    bool box = true;
    bool line = true;
    bool health = true;
    bool distance = true;
    bool name = true;
    bool playerId = false;
    bool armor = true;
    bool counter = true;
    bool circle = false;
    bool box3d = false;
    bool alert = false;
    bool vehicle = false;
    bool ignoreKnocked = false;
    int boxStyle = 0;
    int linePos = 0;
};

void codm_reset();
void codm_set_want_pid(pid_t pid);
pid_t codm_want_pid();
CodmFrame codm_tick(const CodmFeatures &feat);
