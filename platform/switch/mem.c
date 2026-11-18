// Окно гостевой памяти на Switch (спайк S2): один блок кучи RT_WINDOW (1 ГБ), выровненный на 64 КБ.
// Подкачки по требованию у Horizon нет — память занята сразу, поэтому нужен полный режим (запуск через игру
// с зажатой R, ~3,2 ГБ); в applet-режиме (~400 МБ) блок не выделится. Первые 64 КБ гостя (NULL и окрестности)
// закрываются, если ядро позволяет; адреса за окном не охраняются (на маке — резерв 4 ГБ).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "runtime/rt.h"

int rt_platform_mem_init(void)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    AppletType at = appletGetAppletType();
    fprintf(stderr, "[mem] process memory %llu MB (used %llu MB), %s\n", (unsigned long long)(total >> 20),
            (unsigned long long)(used >> 20),
            at == AppletType_Application || at == AppletType_SystemApplication ? "full mode" : "applet mode");
    u8 *p = aligned_alloc(0x10000, RT_WINDOW);
    if (!p) {
        fprintf(stderr, "[mem] cannot allocate the %u MB guest window: launch through a game (hold R) for full "
                        "memory\n", RT_WINDOW >> 20);
        return -1;
    }
    memset(p, 0, RT_WINDOW);
    Result r = svcSetMemoryPermission(p, 0x10000, Perm_None);
    fprintf(stderr, "[mem] guest window at %p, NULL guard %s\n", (void *)p, R_SUCCEEDED(r) ? "on" : "off");
    g_mem = p;
    return 0;
}

void rt_platform_thread_core(int core)
{
    Result r = svcSetThreadCoreMask(threadGetCurHandle(), core, 1u << core);
    if (R_FAILED(r))
        fprintf(stderr, "[mem] svcSetThreadCoreMask(core %d) failed: %#x\n", core, r);
}
