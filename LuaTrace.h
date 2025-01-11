#ifndef LUATRACE_H
#define LUATRACE_H

// https://en.wikipedia.org/wiki/Calling_convention
// http://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64
// http://infocenter.arm.com/help/topic/com.arm.doc.ihi0055b/IHI0055B_aapcs64.pdf

#include <stddef.h>
#include <signal.h>

#ifdef __cplusplus
#include <lua.hpp>
#else
#include <lua.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif
#ifdef __x86_64__

#define TLC_TRACEABLE

int __attribute__((sysv_abi)) MakeTraceableLuaCall(long method, long arguments, long results, long function, long dummy1, long dummy2, lua_State* state);

#ifndef LUATRACE_C
#define lua_call(state, arguments, results)             MakeTraceableLuaCall(TLC_CALL,   arguments, results, 0,        0, 0, state)
#define lua_pcall(state, arguments, results, function)  MakeTraceableLuaCall(TLC_PCALL,  arguments, results, function, 0, 0, state)
#define lua_resume(state, arguments)                    MakeTraceableLuaCall(TLC_RESUME, arguments, 0,       0,        0, 0, state)
#endif

#endif

#ifdef __aarch64__

#define TLC_TRACEABLE

int MakeTraceableLuaCall(long method, long arguments, long results, long function, long dummy1, long dummy2, long dummy3, long dummy4, lua_State* state);

#ifndef LUATRACE_C
#define lua_call(state, arguments, results)             MakeTraceableLuaCall(TLC_CALL,   arguments, results, 0,        0, 0, 0, 0, state)
#define lua_pcall(state, arguments, results, function)  MakeTraceableLuaCall(TLC_PCALL,  arguments, results, function, 0, 0, 0, 0, state)
#define lua_resume(state, arguments)                    MakeTraceableLuaCall(TLC_RESUME, arguments, 0,       0,        0, 0, 0, 0, state)
#endif

#endif

#ifdef __arm__

#define TLC_TRACEABLE

int MakeTraceableLuaCall(long method, long arguments, long results, long function, lua_State* state);

#ifndef LUATRACE_C
#define lua_call(state, arguments, results)             MakeTraceableLuaCall(TLC_CALL,   arguments, results, 0,        state)
#define lua_pcall(state, arguments, results, function)  MakeTraceableLuaCall(TLC_PCALL,  arguments, results, function, state)
#define lua_resume(state, arguments)                    MakeTraceableLuaCall(TLC_RESUME, arguments, 0,       0,        state)
#endif

#endif

#ifdef TLC_TRACEABLE

#define TLC_CALL    0
#define TLC_PCALL   1
#define TLC_RESUME  2

lua_State* GetLuaStateOnStack();

#endif

typedef void (*LuaTraceReportFunction)(int priority, const char* format, ...);

int GetLuaTraceBack(lua_State* state, char* buffer, size_t size);
int MakeLuaTraceReport(siginfo_t* information, LuaTraceReportFunction report);

#ifdef __cplusplus
}
#endif

#endif