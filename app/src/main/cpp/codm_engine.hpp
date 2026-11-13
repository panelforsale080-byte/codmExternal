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
    bool visible = true;
    int team = -1;
    char name[32]{};
    Vec3 headWorld{};
    Vec3 skel[6]{};
    bool skelOk[6]{};
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
    bool skeleton = false;
    bool silentAim = false;
    bool ignoreWall = false;
    bool disableVis = false;
    bool handCam = false;
    bool wallhack = false;
    bool maphack = false;
    bool noParachute = false;
    bool fastSwim = false;
    bool hitbox = false;
    bool noShake = false;
    bool noCrouch = false;
    bool misdirection = false;
    bool frame120 = false;
    bool frame40 = false;
    bool skipTutorial = false;
    int boxStyle = 0;
    int linePos = 0;
    int targetMode = 1;
    int priority = 0;
    int trigger = 0;
    float aimSmooth = 50.f;
    float aimSpeed = 50.f;
    float aimFov = 120.f;
    float recoil = 0.f;
    float ads = 0.f;
    float reload = 0.f;
    float spread = 0.f;
    float aimRange = 150.f;
    float aimAssist = 0.f;
    float fovBr = 0.f;
    float fovMp = 0.f;
    float snow = 0.f;
    float loot = 0.f;
    float gameSpeed = 0.f;
    float screenW = 0.f;
    float screenH = 0.f;
    float colEnemy[4] = {1.f, 0.f, 0.f, 1.f};
    float colBot[4] = {0.f, 1.f, 0.f, 1.f};
    float colHidden[4] = {1.f, 1.f, 1.f, 1.f};
    float colKnocked[4] = {1.f, 0.65f, 0.f, 1.f};
    float colDefault[4] = {1.f, 1.f, 1.f, 1.f};
    struct {
        bool hitbox = false;
        bool noShake = false;
        float assist = 0.f;
        float recoil = 0.f;
        float reload = 0.f;
        float spread = 0.f;
        float ads = 0.f;
    } weapon[7]{};
};

void codm_reset();
void codm_set_want_pid(pid_t pid);
pid_t codm_want_pid();
CodmFrame codm_tick(const CodmFeatures &feat);
