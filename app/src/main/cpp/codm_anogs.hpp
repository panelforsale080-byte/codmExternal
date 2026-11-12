#pragma once
#include <sys/types.h>

/** Apply jnmaini hooks.h PATCH (mov x0,#0; ret) at libanogs + RVAs. vm write, else ptrace. */
bool codm_anogs_auto_patch(pid_t pid, uintptr_t anogsBase, char *err, size_t errCap);
bool codm_anogs_is_patched(pid_t pid);
void codm_anogs_reset_patch_cache();
void codm_anogs_force_repatch();
