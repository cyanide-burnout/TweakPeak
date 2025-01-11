#ifndef WATCHPOINT_H
#define WATCHPOINT_H

#include <stdint.h>
#include <stddef.h>
#include <signal.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Note: handler for SIGTRAP should be installed first

#if defined(__i386__) || defined(__x86_64__)

#define WATCHPOINT_BREAK_ON_EXECUTE    (0b00 << 16)
#define WATCHPOINT_BREAK_ON_WRITE      (0b01 << 16)
#define WATCHPOINT_BREAK_ON_READWRITE  (0b11 << 16)

#define WATCHPOINT_LENGTH_BYTE         (0b00 << 18)
#define WATCHPOINT_LENGTH_WORD         (0b01 << 18)
#define WATCHPOINT_LENGTH_DWORD        (0b11 << 18)
#define WATCHPOINT_LENGTH_QWORD        (0b10 << 18)

#endif
#if defined(__arm__) || defined(__aarch64__)

#define WATCHPOINT_BREAK_ON_EXECUTE    (0b00 << 3)
#define WATCHPOINT_BREAK_ON_READ       (0b01 << 3)
#define WATCHPOINT_BREAK_ON_WRITE      (0b10 << 3)
#define WATCHPOINT_BREAK_ON_READWRITE  (0b11 << 3)

#define WATCHPOINT_LENGTH_BYTE         ((sizeof(uint8_t)  - 1) << 5)
#define WATCHPOINT_LENGTH_WORD         ((sizeof(uint16_t) - 1) << 5)
#define WATCHPOINT_LENGTH_DWORD        ((sizeof(uint32_t) - 1) << 5)
#define WATCHPOINT_LENGTH_QWORD        ((sizeof(uint64_t) - 1) << 5)

#endif

typedef void (*WatchPointReportFunction)(int priority, const char* format, ...);

void TerminateWatch();
int SetWatchPoint(int number, const void* address, uint32_t condition);
int GetWatchPoint();

int MakeWatchPointReport(siginfo_t* information, WatchPointReportFunction report);

#ifdef __cplusplus
};
#endif

#endif
