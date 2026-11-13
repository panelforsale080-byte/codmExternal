#include "remote_call.hpp"
#include "mem.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <mutex>
#include <thread>
#include <dirent.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__aarch64__)

#include <linux/elf.h>
#include <sys/user.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_PRFPREG
#define NT_PRFPREG 2
#endif

struct CodmFpsimd {
    uint64_t v[64]{};
    uint32_t fpsr = 0, fpcr = 0, pad0 = 0, pad1 = 0;
};

static CodmFpsimd *saved_fp(RemoteSession &s) {
    return reinterpret_cast<CodmFpsimd *>(s.savedFp);
}

static bool get_fp(pid_t tid, CodmFpsimd &fp) {
    struct iovec iov{&fp, sizeof(fp)};
    if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRFPREG, &iov) == 0) return true;
    iov.iov_base = &fp;
    iov.iov_len = sizeof(fp);
    return ptrace(PTRACE_GETREGSET, tid, (void *)0x202, &iov) == 0;
}

static bool set_fp(pid_t tid, const CodmFpsimd &fp) {
    struct iovec iov{const_cast<CodmFpsimd *>(&fp), sizeof(fp)};
    if (ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRFPREG, &iov) == 0) return true;
    iov.iov_base = const_cast<CodmFpsimd *>(&fp);
    iov.iov_len = sizeof(fp);
    return ptrace(PTRACE_SETREGSET, tid, (void *)0x202, &iov) == 0;
}

static float fp_lane(const CodmFpsimd &fp, int reg) {
    float f = 0;
    std::memcpy(&f, &fp.v[reg * 2], sizeof(f));
    return f;
}

static void fp_set_lane(CodmFpsimd &fp, int reg, float f) {
    std::memset(&fp.v[reg * 2], 0, 16);
    std::memcpy(&fp.v[reg * 2], &f, sizeof(f));
}

static std::mutex g_ptraceSessionMu;

/** Unity player loop — same thread jnmaini calls get_MatchGame from. */
static pid_t pick_tracee_tid(pid_t pid) {
    const std::string dir = "/proc/" + std::to_string(pid) + "/task";
    DIR *d = opendir(dir.c_str());
    if (!d) return pid;
    pid_t unity = 0;
    pid_t gfx = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
        std::ifstream in(dir + "/" + e->d_name + "/comm");
        std::string comm;
        std::getline(in, comm);
        if (comm.find("UnityMain") != std::string::npos) unity = tid;
        else if (comm.find("UnityGfx") != std::string::npos && !gfx) gfx = tid;
    }
    closedir(d);
    if (unity) return unity;
    if (gfx) return gfx;
    return pid;
}

static struct user_pt_regs *saved_pt(RemoteSession &s) {
    return reinterpret_cast<struct user_pt_regs *>(s.savedRegs);
}

static bool get_regs(pid_t tid, struct user_pt_regs &regs) {
    struct iovec iov{&regs, sizeof(regs)};
    return ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &iov) == 0;
}

static bool set_regs(pid_t tid, const struct user_pt_regs &regs) {
    struct iovec iov{const_cast<struct user_pt_regs *>(&regs), sizeof(regs)};
    return ptrace(PTRACE_SETREGSET, tid, (void *)NT_PRSTATUS, &iov) == 0;
}

static bool wait_stopped(pid_t tid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    int st = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t w = waitpid(tid, &st, WNOHANG);
        if (w == tid && WIFSTOPPED(st)) return true;
        if (w < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

static bool stable_ptrace_attach(pid_t tid) {
    if (ptrace(PTRACE_SEIZE, tid, nullptr, (void *)PTRACE_O_TRACESYSGOOD) != 0) return false;
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) {
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }
    if (wait_stopped(tid, 80)) return true;
    /* A blocking wait here freezes UnityMain and leaves the menu on Waiting. */
    ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
    errno = ETIMEDOUT;
    return false;
}

static bool ensure_stopped(pid_t tid) {
    int st = 0;
    if (waitpid(tid, &st, WNOHANG) == tid && WIFSTOPPED(st)) return true;
    if (ptrace(PTRACE_INTERRUPT, tid, nullptr, nullptr) != 0) return false;
    return wait_stopped(tid, 40);
}

static bool session_begin_locked(pid_t pid, RemoteSession &s) {
    s = RemoteSession{};
    s.pid = pid;
    s.tid = pick_tracee_tid(pid);
    if (!stable_ptrace_attach(s.tid)) {
        std::snprintf(s.err, sizeof(s.err), "PTRACE_SEIZE: %s", std::strerror(errno));
        return false;
    }
    auto *saved = saved_pt(s);
    if (!get_regs(s.tid, *saved) && !(ensure_stopped(s.tid) && get_regs(s.tid, *saved))) {
        std::snprintf(s.err, sizeof(s.err), "GETREGSET fail");
        ptrace(PTRACE_DETACH, s.tid, nullptr, nullptr);
        return false;
    }
    s.sp = saved->sp;
    /* mlbb-pure only keeps GP regs. MatchGame returns x0, so a missing FP set must not abort. */
    s.fpSaved = get_fp(s.tid, *saved_fp(s));
    s.attached = true;
    return true;
}

bool remote_session_begin(pid_t pid, RemoteSession &s) {
    g_ptraceSessionMu.lock();
    if (!session_begin_locked(pid, s)) {
        g_ptraceSessionMu.unlock();
        return false;
    }
    return true;
}

bool remote_session_begin_try(pid_t pid, RemoteSession &s) {
    if (!g_ptraceSessionMu.try_lock()) {
        std::snprintf(s.err, sizeof(s.err), "ptrace busy");
        return false;
    }
    if (!session_begin_locked(pid, s)) {
        g_ptraceSessionMu.unlock();
        return false;
    }
    return true;
}

void remote_session_end(RemoteSession &s) {
    if (s.attached) {
        ensure_stopped(s.tid);
        if (s.fpSaved) set_fp(s.tid, *saved_fp(s));
        if (!set_regs(s.tid, *saved_pt(s))) set_regs(s.tid, *saved_pt(s));
        /* data = 0: do not deliver SIGSEGV left by a bad return. */
        ptrace(PTRACE_DETACH, s.tid, nullptr, nullptr);
        s.attached = false;
    }
    g_ptraceSessionMu.unlock();
}

uintptr_t remote_session_scratch(RemoteSession &s, int slot, size_t bytes) {
    if (!s.attached || slot < 0 || slot > 32) return 0;
    const size_t need = (bytes + 15) & ~size_t(15);
    const uintptr_t base = (uintptr_t)(s.sp - 0x800ULL - (uintptr_t)slot * 0x80ULL);
    if (need > 0x70) return 0;
    std::vector<uint8_t> z(need, 0);
    if (!mem_write(s.pid, base, z.data(), need)) return 0;
    return base;
}

bool remote_session_call(RemoteSession &s, uintptr_t func, const std::vector<uintptr_t> &args,
                         uintptr_t &ret_x0) {
    ret_x0 = 0;
    if (!s.attached || !func) {
        std::snprintf(s.err, sizeof(s.err), "session/call invalid");
        return false;
    }
    /* Spin here on return. LR=0 jumps to address 0 and SIGSEGV-kills the game. */
    const uintptr_t stub = remote_session_scratch(s, 31, 16);
    const uint32_t spin = 0x14000000u; /* b . */
    if (!stub || !mem_write(s.pid, stub, &spin, sizeof(spin))) {
        std::snprintf(s.err, sizeof(s.err), "return stub fail");
        return false;
    }
    struct user_pt_regs work = *saved_pt(s);
    for (size_t i = 0; i < args.size() && i < 8; ++i)
        work.regs[i] = args[i];
    work.sp = saved_pt(s)->sp & ~uintptr_t{0xF};
    work.pc = func;
    work.regs[30] = stub;
    if (!set_regs(s.tid, work)) {
        std::snprintf(s.err, sizeof(s.err), "SETREGSET fail");
        return false;
    }
    ptrace(PTRACE_CONT, s.tid, nullptr, nullptr);
    int st = 0;
    bool success = false;
    bool fatal = false;
    /* b . does not trap, so sample PC. A fault stop is restored and not delivered. */
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if (waitpid(s.tid, &st, WNOHANG) == s.tid && WIFSTOPPED(st)) {
            const int sig = WSTOPSIG(st);
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
                fatal = true;
                std::snprintf(s.err, sizeof(s.err), "call signal %d", sig);
                break;
            }
        }
        if (!ensure_stopped(s.tid) || !get_regs(s.tid, work)) break;
        if (work.pc == stub || work.pc == stub + 4) {
            success = true;
            break;
        }
        ptrace(PTRACE_CONT, s.tid, nullptr, nullptr);
    }
    if (!ensure_stopped(s.tid) || !get_regs(s.tid, work)) {
        std::snprintf(s.err, sizeof(s.err), "GETREGSET after call");
        set_regs(s.tid, *saved_pt(s));
        return false;
    }
    if (work.pc == stub || work.pc == stub + 4) {
        success = true;
        ret_x0 = (uintptr_t)work.regs[0];
    } else if (!fatal) {
        std::snprintf(s.err, sizeof(s.err), "ptrace call timeout");
    }
    if (!set_regs(s.tid, *saved_pt(s))) {
        std::snprintf(s.err, sizeof(s.err), "restore regs fail");
        return false;
    }
    return success && !fatal;
}

bool remote_session_call_f(RemoteSession &s, uintptr_t func, uintptr_t x0, const float *in, int inN,
                           float *out, int outN) {
    if (!s.attached || !func) return false;
    if (in && inN > 0) {
        CodmFpsimd fp = *saved_fp(s);
        const int n = inN > 8 ? 8 : inN;
        for (int i = 0; i < n; ++i) fp_set_lane(fp, i, in[i]);
        if (!set_fp(s.tid, fp)) return false;
    }
    uintptr_t dummy = 0;
    std::vector<uintptr_t> args;
    if (x0) args.push_back(x0);
    if (!remote_session_call(s, func, args, dummy)) return false;
    if (out && outN > 0) {
        CodmFpsimd fp{};
        if (!get_fp(s.tid, fp)) return false;
        const int n = outN > 8 ? 8 : outN;
        for (int i = 0; i < n; ++i) out[i] = fp_lane(fp, i);
    }
    return true;
}

bool remote_session_call_vec3(RemoteSession &s, uintptr_t func, uintptr_t x0, const Vec3 *in,
                              Vec3 &out) {
    out = {};
    if (!s.attached || !func) return false;
    CodmFpsimd fp = *saved_fp(s);
    if (in) {
        fp_set_lane(fp, 0, in->x);
        fp_set_lane(fp, 1, in->y);
        fp_set_lane(fp, 2, in->z);
    }
    if (!set_fp(s.tid, fp)) return false;
    uintptr_t dummy = 0;
    if (!remote_session_call(s, func, {x0}, dummy)) return false;
    if (!get_fp(s.tid, fp)) return false;
    out.x = fp_lane(fp, 0);
    out.y = fp_lane(fp, 1);
    out.z = fp_lane(fp, 2);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool remote_call(pid_t pid, uintptr_t func, const std::vector<uintptr_t> &args, uintptr_t &ret_x0,
                 char *err, size_t errCap) {
    RemoteSession s{};
    if (!remote_session_begin(pid, s)) {
        std::snprintf(err, errCap, "%s", s.err);
        return false;
    }
    const bool ok = remote_session_call(s, func, args, ret_x0);
    if (!ok) std::snprintf(err, errCap, "%s", s.err);
    remote_session_end(s);
    return ok;
}

#else

bool remote_session_begin(pid_t, RemoteSession &s) {
    std::snprintf(s.err, sizeof(s.err), "arm64 only");
    return false;
}
bool remote_session_begin_try(pid_t, RemoteSession &s) {
    std::snprintf(s.err, sizeof(s.err), "arm64 only");
    return false;
}
void remote_session_end(RemoteSession &) {}
uintptr_t remote_session_scratch(RemoteSession &, int, size_t) { return 0; }
bool remote_session_call(RemoteSession &, uintptr_t, const std::vector<uintptr_t> &, uintptr_t &) {
    return false;
}
bool remote_session_call_vec3(RemoteSession &, uintptr_t, uintptr_t, const Vec3 *, Vec3 &) {
    return false;
}
bool remote_session_call_f(RemoteSession &, uintptr_t, uintptr_t, const float *, int, float *, int) {
    return false;
}
bool remote_call(pid_t, uintptr_t, const std::vector<uintptr_t> &, uintptr_t &, char *err,
                 size_t errCap) {
    std::snprintf(err, errCap, "arm64 only");
    return false;
}

#endif
