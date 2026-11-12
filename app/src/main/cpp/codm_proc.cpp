#include "codm_proc.hpp"
#include "mem.hpp"
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <string>

static const char *kTargetPkg = "com.garena.game.codm";

static std::string read_cmdline(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!f) return {};
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (char &c : all)
        if (c == '\0') c = ' ';
    while (!all.empty() && all.back() == ' ') all.pop_back();
    return all;
}

bool proc_alive(pid_t pid) { return pid > 0 && kill(pid, 0) == 0; }

bool mem_module_base(pid_t pid, const char *soname, uintptr_t &base) {
    base = 0;
    if (!soname || !soname[0]) return false;
    std::vector<MapsReg> maps;
    if (!mem_maps(pid, maps)) return false;
    const std::string suffix = std::string("/") + soname;
    for (const auto &r : maps) {
        if (r.path.size() < suffix.size()) continue;
        if (r.path.compare(r.path.size() - suffix.size(), suffix.size(), suffix) != 0 &&
            r.path.find(soname) == std::string::npos)
            continue;
        if (r.perms[0] != 'r' || r.perms[2] != 'x') continue;
        if (!base || r.start < base) base = r.start;
    }
    return base != 0;
    return false;
}

static bool maps_has(pid_t pid, const char *soname) {
    uintptr_t b = 0;
    return mem_module_base(pid, soname, b);
}

size_t proc_scan_codm(CodmProcHit *out, size_t maxOut) {
    if (!out || !maxOut) return 0;
    DIR *d = opendir("/proc");
    if (!d) return 0;
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        const pid_t pid = static_cast<pid_t>(std::atoi(e->d_name));
        if (pid <= 0 || !proc_alive(pid)) continue;
        const std::string cmd = read_cmdline(pid);
        if (cmd.empty()) continue;
        const size_t sp = cmd.find(' ');
        const std::string pkg = sp == std::string::npos ? cmd : cmd.substr(0, sp);
        if (pkg != kTargetPkg) continue;
        CodmProcHit h{};
        h.pid = pid;
        h.unity = maps_has(pid, "libunity.so");
        h.anogs = maps_has(pid, "libanogs.so");
        h.score = 100;
        if (h.unity) h.score += 100;
        if (h.anogs) h.score += 90;
        std::snprintf(h.cmdline, sizeof(h.cmdline), "%s", cmd.c_str());
        out[n++] = h;
        if (n >= maxOut) break;
    }
    closedir(d);
    return n;
}

pid_t proc_best_codm() {
    CodmProcHit hits[32];
    const size_t n = proc_scan_codm(hits, 32);
    pid_t best = 0;
    int bestScore = 0;
    for (size_t i = 0; i < n; ++i) {
        if (hits[i].score > bestScore) {
            bestScore = hits[i].score;
            best = hits[i].pid;
        }
    }
    return best;
}
