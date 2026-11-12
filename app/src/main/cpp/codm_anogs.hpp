#pragma once
#include <sys/types.h>

/** Apply jnmaini PATCH bytes at libanogs_base + RVAs via process_vm_writev. Idempotent per pid. */
bool codm_anogs_auto_patch(pid_t pid, uintptr_t anogsBase, char *err, size_t errCap);
void codm_anogs_reset_patch_cache();
