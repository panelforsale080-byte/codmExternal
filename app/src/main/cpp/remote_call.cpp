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
#ifndef NT_ARM_SYSTEM_CALL
#define NT_ARM_SYSTEM_CALL 0x404
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

static std::string thread_comm(pid_t pid, pid_t tid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm");
    std::string comm;
    std::getline(in, comm);
    while (!comm.empty() && (comm.back() == '\r' || comm.back() == ' ')) comm.pop_back();
    return comm;
}

/** -1 in /proc/tid/syscall means the thread is in userspace. A kernel restart ignores a new PC. */
static bool thread_in_kernel(pid_t pid, pid_t tid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/syscall");
    std::string line;
    if (!std::getline(in, line)) return false;
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) return false;
    const bool userspace = line[i] == '-' && i + 1 < line.size() && line[i + 1] == '1' &&
                           (i + 2 >= line.size() || (line[i + 2] < '0' || line[i + 2] > '9'));
    return !userspace;
}

/** jnmaini calls get_MatchGame on UnityMain. The process main thread is usually blocked
    in a syscall, so its x0 is -EINTR and is not a MatchGame pointer. */
static pid_t pick_tracee_tid(pid_t pid, char *names, size_t namesCap) {
    if (names && namesCap) names[0] = '\0';
    const std::string dir = "/proc/" + std::to_string(pid) + "/task";
    DIR *d = opendir(dir.c_str());
    pid_t mainUser = 0;
    pid_t mainAny = 0;
    pid_t otherUser = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            const pid_t tid = static_cast<pid_t>(std::atoi(e->d_name));
            if (tid == pid) continue;
            const std::string comm = thread_comm(pid, tid);
            const bool isMain = comm.find("UnityMain") != std::string::npos;
            const bool isUnity = isMain || comm.find("Unity") != std::string::npos;
            if (!isUnity) {
                if (names && namesCap && std::strlen(names) + comm.size() + 1 < namesCap && !comm.empty()) {
                    if (names[0]) std::strncat(names, ",", namesCap - std::strlen(names) - 1);
                    std::strncat(names, comm.c_str(), namesCap - std::strlen(names) - 1);
                }
                continue;
            }
            const bool user = !thread_in_kernel(pid, tid);
            if (isMain && user) mainUser = tid;
            else if (isMain && !mainAny) mainAny = tid;
            else if (user && !otherUser) otherUser = tid;
        }
        closedir(d);
    }
    if (mainUser) return mainUser;
    if (otherUser) return otherUser;
    return mainAny;
}

/** Plain ret, or PAC retaa/retab/autiasp. x0 is already the return value. */
static bool insn_is_return(uint32_t in) {
    return in == 0xD65F03C0u || in == 0xD65F0BFFu || in == 0xD65F0FFFu || in == 0xD50323BFu ||
           in == 0xD50323FFu;
}

static bool get_syscallno(pid_t tid, int &no) {
    struct iovec iov{&no, sizeof(no)};
    return ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_ARM_SYSTEM_CALL, &iov) == 0;
}

static bool set_syscallno(pid_t tid, int no) {
    struct iovec iov{&no, sizeof(no)};
    return ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_ARM_SYSTEM_CALL, &iov) == 0;
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
    char others[120] = {};
    s.tid = pick_tracee_tid(pid, others, sizeof(others));
    if (s.tid <= 0) {
        std::snprintf(s.err, sizeof(s.err), "no UnityMain thread%s%s", others[0] ? " " : "", others);
        return false;
    }
    const std::string comm = thread_comm(pid, s.tid);
    std::snprintf(s.comm, sizeof(s.comm), "%s", comm.c_str());
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
        if (s.fpSaved) set_fp(s.tid, *saved_fp(s));
        set_regs(s.tid, *saved_pt(s));
        /* data = 0: do not deliver the SIGSEGV from a return to address 0. */
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
    int savedNo = -1;
    const bool hadNo = get_syscallno(s.tid, savedNo);
    struct user_pt_regs work = *saved_pt(s);
    const uintptr_t originPc = work.pc & 0x00FFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < args.size() && i < 8; ++i)
        work.regs[i] = args[i];
    work.sp = saved_pt(s)->sp & ~uintptr_t{0xF};
    work.pc = func;
    work.regs[30] = 0;
    if (!set_regs(s.tid, work)) {
        std::snprintf(s.err, sizeof(s.err), "SETREGSET fail");
        return false;
    }
    /* syscallno >= 0 makes the kernel ignore the new PC and restart the old call. */
    int neg = -1;
    set_syscallno(s.tid, neg);
    ptrace(PTRACE_CONT, s.tid, nullptr, nullptr);
    bool got = false;
    int lastSig = 0;
    uintptr_t lastPc = 0;
    bool saw = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline && !got && !s.err[0]) {
        int st = 0;
        const pid_t w = waitpid(s.tid, &st, WNOHANG);
        if (w == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (w < 0 && errno != EINTR) break;
        if (w != s.tid || !WIFSTOPPED(st)) continue;
        const int sig = WSTOPSIG(st);
        lastSig = sig;
        if (sig == (SIGTRAP | 0x80)) {
            ptrace(PTRACE_CONT, s.tid, nullptr, nullptr);
            continue;
        }
        if (!get_regs(s.tid, work)) break;
        const uintptr_t pc = (uintptr_t)work.pc & 0x00FFFFFFFFFFFFFFULL;
        lastPc = pc;
        saw = true;
        uint32_t in = 0;
        const bool atRet = pc >= 0x10000 && mem_read(s.pid, pc, in) && insn_is_return(in);
        const bool fault = sig == SIGSEGV || sig == SIGBUS || sig == SIGILL;
        if (fault && (pc < 0x1000 || atRet)) {
            const uintptr_t x0 = (uintptr_t)work.regs[0];
            /* Return-to-0 from a restarted syscall still holds -EINTR. A ret in the callee does not. */
            if (pc < 0x1000 && !atRet && x0 > (uintptr_t)-4096) {
                std::snprintf(s.err, sizeof(s.err), "syscall x0");
                break;
            }
            ret_x0 = x0;
            got = true;
            break;
        }
        if (fault) {
            std::snprintf(s.err, sizeof(s.err), "call fault sig=%d pc=0x%llx", sig,
                          (unsigned long long)pc);
            break;
        }
        if (pc == originPc) {
            std::snprintf(s.err, sizeof(s.err), "pc not applied");
            break;
        }
        ptrace(PTRACE_CONT, s.tid, nullptr, nullptr);
    }
    if (!got && !s.err[0]) {
        ptrace(PTRACE_INTERRUPT, s.tid, nullptr, nullptr);
        if (wait_stopped(s.tid, 60) && get_regs(s.tid, work)) {
            const uintptr_t pc = (uintptr_t)work.pc & 0x00FFFFFFFFFFFFFFULL;
            if (pc == originPc)
                std::snprintf(s.err, sizeof(s.err), "pc not applied");
            else
                std::snprintf(s.err, sizeof(s.err), "timeout sig=%d pc=0x%llx", lastSig,
                              (unsigned long long)(saw ? lastPc : pc));
        } else if (!s.err[0]) {
            std::snprintf(s.err, sizeof(s.err), "ptrace call timeout");
        }
    } else if (!got) {
        ptrace(PTRACE_INTERRUPT, s.tid, nullptr, nullptr);
        wait_stopped(s.tid, 40);
    }
    if (hadNo) set_syscallno(s.tid, savedNo);
    else set_syscallno(s.tid, neg);
    if (!set_regs(s.tid, *saved_pt(s))) {
        ptrace(PTRACE_INTERRUPT, s.tid, nullptr, nullptr);
        wait_stopped(s.tid, 80);
        if (!set_regs(s.tid, *saved_pt(s))) {
            std::snprintf(s.err, sizeof(s.err), "restore regs fail");
            return false;
        }
    }
    return got;
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
