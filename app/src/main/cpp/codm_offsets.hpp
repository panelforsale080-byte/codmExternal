#pragma once
#include <cstdint>

/** CODM libunity hardcoded RVAs — game version aligned with jnmaini Dump 1.6.59 / CLASS.h */
namespace CodmOff {

constexpr uintptr_t kMatchGame = 0x801191C;
constexpr uintptr_t kLocalPawn = 0x8012814;
constexpr uintptr_t kCameraMain = 0x52D5CDC;
constexpr uintptr_t kWorldToScreen = 0x52D5820;
constexpr uintptr_t kTransformPos = 0x532DF40;
constexpr uintptr_t kPawnLastPos = 0x538AABC;
constexpr uintptr_t kPawnHeadPos = 0x538A478;
constexpr uintptr_t kGetTransform = 0x52DAEC4;
constexpr uintptr_t kTeamSeat = 0x538A780;
constexpr uintptr_t kVehicles = 0x1A0;
constexpr uintptr_t kCanSee = 0x9A0;
constexpr uintptr_t kBoneMap = 0xA8;
constexpr uintptr_t kBoneTable = 0x10;
constexpr uintptr_t kBoneNode = 0x40;
constexpr uintptr_t kAimGet = 0x5385F80;
constexpr uintptr_t kAimSet = 0x5386048;
constexpr uintptr_t kIsFiring = 0x5379778;
constexpr uintptr_t kIsAiming = 0x536BAE0;
constexpr uintptr_t kForward = 0x534ECCC;
constexpr uintptr_t kCurrentWeapon = 0x535BE4C;
constexpr uintptr_t kCategoryName = 0xC0D0730;
constexpr uintptr_t kWallhack = 0x5565680;

constexpr uintptr_t kEnemyPawns = 0x178;

constexpr uintptr_t kPawnIsAlive = 0x550;
constexpr uintptr_t kPawnMesh = 0x630;
constexpr uintptr_t kPawnHeadBone = 0x310;
constexpr uintptr_t kPawnIsBot = 0x5C1;
constexpr uintptr_t kPawnPlayerInfo = 0x5C8;
constexpr uintptr_t kPlayerState = 0x424;
constexpr uintptr_t kVest = 0x608;
constexpr uintptr_t kVestMax = 0x60C;
constexpr uintptr_t kPlayerName = 0x538A968;
constexpr int kKnocked = 4;
constexpr uintptr_t kAtkInfo = 0x78;
constexpr uintptr_t kHealth = 0x34;
constexpr uintptr_t kHealthMax = 0x38;

constexpr uintptr_t kIl2ListItems = 0x10;
constexpr uintptr_t kIl2ListSize = 0x18;
constexpr uintptr_t kIl2ArrayData = 0x20;

/** libanogs.so — do not change (jnmaini hooks.h) */
constexpr uintptr_t kAnogsPatch[] = {0x204218, 0x45B334, 0x44A714, 0x48F964};
constexpr size_t kAnogsPatchCount = 4;

} // namespace CodmOff
