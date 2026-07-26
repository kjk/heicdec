// Vendored from https://github.com/kjk/winperf client/winperf_control.h.
// Drop-in, single-header control for winperf "section" profiling.
#ifndef WINPERF_CONTROL_H
#define WINPERF_CONTROL_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire format: one fixed 13-byte record per mark. Keep in sync with
 * WinperfMark in winperf's src/Winperf.h. */
#pragma pack(push, 1)
typedef struct WinperfMark_ {
    uint8_t opcode; /* 0 = stop, 1 = start */
    uint32_t pid;
    uint64_t qpc;
} WinperfMark_;
#pragma pack(pop)

static void winperf__send(uint8_t opcode)
{
    /* INVALID_HANDLE_VALUE = not tried, NULL = tried and disabled. */
    static volatile HANDLE s_pipe = INVALID_HANDLE_VALUE;
    static volatile LONG s_connecting = 0;
    HANDLE h = s_pipe;

    if (h == INVALID_HANDLE_VALUE) {
        if (InterlockedCompareExchange(&s_connecting, 1, 0) != 0)
            return;
        h = CreateFileW(L"\\\\.\\pipe\\winperf-control", GENERIC_WRITE, 0,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = NULL;
        s_pipe = h;
    }
    if (h == NULL)
        return;

    {
        WinperfMark_ m;
        LARGE_INTEGER qpc;
        DWORD wrote = 0;
        QueryPerformanceCounter(&qpc);
        m.opcode = opcode;
        m.pid = (uint32_t)GetCurrentProcessId();
        m.qpc = (uint64_t)qpc.QuadPart;
        WriteFile(h, &m, (DWORD)sizeof(m), &wrote, NULL);
    }
}

static void winperf_profile_start(void)
{
    winperf__send(1);
}

static void winperf_profile_stop(void)
{
    winperf__send(0);
}

#ifdef __cplusplus
} /* extern "C" */

struct WinperfProfileRegion {
    WinperfProfileRegion() { winperf_profile_start(); }
    ~WinperfProfileRegion() { winperf_profile_stop(); }
};
#endif

#endif /* WINPERF_CONTROL_H */
