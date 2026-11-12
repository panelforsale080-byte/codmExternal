#pragma once
#include <cstddef>
#include <sys/types.h>

struct CodmProcHit {
    pid_t pid = 0;
    int score = 0;
    bool unity = false;
    bool anogs = false;
    char cmdline[280]{};
};

bool proc_alive(pid_t pid);
bool mem_module_base(pid_t pid, const char *soname, uintptr_t &base);
size_t proc_scan_codm(CodmProcHit *out, size_t maxOut);
pid_t proc_best_codm();
