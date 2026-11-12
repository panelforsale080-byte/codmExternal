#include "codm_remote.hpp"
#include "remote_call.hpp"
#include <cmath>
#include <cstring>

bool codm_call_u64(pid_t pid, uintptr_t fn, const std::vector<uintptr_t> &args, uintptr_t &ret) {
    char err[128] = {};
    return remote_call(pid, fn, args, ret, err, sizeof(err));
}

bool codm_call_vec3(pid_t pid, uintptr_t fn, uintptr_t arg0, Vec3 &out) {
    uintptr_t ret = 0;
    if (!codm_call_u64(pid, fn, {arg0}, ret)) return false;
    if (mem_ptr_ok(ret) && mem_read(pid, ret, out)) return true;
    /* Some builds return raw bits in x0 only — try as inline float bits */
    float fx = 0;
    std::memcpy(&fx, &ret, sizeof(fx));
    if (std::isfinite(fx) && std::fabs(fx) < 1e6f) {
        out.x = fx;
        return true;
    }
    return false;
}

bool codm_world_to_screen(pid_t pid, uintptr_t fnW2s, uintptr_t camera, const Vec3 &world,
                          Vec3 &screen) {
    RemoteSession s{};
    if (!remote_session_begin(pid, s)) return false;
    const uintptr_t arg = remote_session_scratch(s, 0, sizeof(Vec3));
  if (!arg || !mem_write(pid, arg, &world, sizeof(world))) {
        remote_session_end(s);
        return false;
    }
    uintptr_t ret = 0;
    const bool ok = remote_session_call(s, fnW2s, {camera, arg}, ret);
    remote_session_end(s);
    if (!ok) return false;
    if (mem_ptr_ok(ret) && mem_read(pid, ret, screen)) return true;
    Vec3 scratch{};
    if (mem_read(pid, arg, scratch)) {
        screen = scratch;
        return true;
    }
    return false;
}
