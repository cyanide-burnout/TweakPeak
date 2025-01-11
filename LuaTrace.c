#define UNW_LOCAL_ONLY
#define _GNU_SOURCE
#define LUATRACE_C

#include "LuaTrace.h"

#include <stdio.h>
#include <syslog.h>
#include <libunwind.h>

#define BUFFER_LENGTH  8192

#ifdef TLC_TRACEABLE

#ifdef __x86_64__
int __attribute__((optimize("O0"), sysv_abi))
  MakeTraceableLuaCall(long method, long arguments, long results, long function, long dummy1, long dummy2, lua_State* state)
  //                   RDI          RSI             RDX           RCX            R8           R9           [RBP + 16]
#endif
#ifdef __aarch64__
int __attribute__((optimize("O0")))
  MakeTraceableLuaCall(long method, long arguments, long results, long function, long dummy1, long dummy2, long dummy3, long dummy4, lua_State* state)
  //                   R0           R1              R2            R3             R4           R5           R6           R7           [SP + 0]
#endif
#ifdef __arm__
int __attribute__((optimize("O0"), pcs("aapcs")))
  MakeTraceableLuaCall(long method, long arguments, long results, long function, lua_State* state)
  //                   R0           R1              R2            R3             [SP + 0]
#endif
{
  switch (method)
  {
    case TLC_CALL:
      lua_call(state, arguments, results);
      return 0;

    case TLC_PCALL:
      return lua_pcall(state, arguments, results, function);

    case TLC_RESUME:
      return lua_resume(state, arguments);
  }
}

lua_State* GetLuaStateOnStack()
{
  unw_cursor_t cursor;
  unw_context_t context;
  unw_proc_info_t information;
  unw_word_t stack;
  void** arguments;

  unw_getcontext(&context);
  unw_init_local(&cursor, &context);

  while (unw_step(&cursor) > 0)
  {
    unw_get_proc_info(&cursor, &information);
    if ((void*)information.start_ip == MakeTraceableLuaCall)
    {
      // Move to frame of MakeTraceableLuaCall
      unw_step(&cursor);
      unw_get_reg(&cursor, UNW_REG_SP, &stack);  // amd64: RBP + 16, aarch64: SP + 0
      arguments = (void**)stack;
      return (lua_State*)arguments[0];
    }
  }

  return NULL;
}

#endif

int GetLuaTraceBack(lua_State* state, char* buffer, size_t size)
{
  int level;
  size_t length;
  const char* name;
  lua_Debug information;

  level = 0;
  *buffer = '\0';

  while (lua_getstack(state, level, &information) != 0)
  {
    lua_getinfo(state, "nSl", &information);

    level ++;
    name = information.name;

    if (name == NULL)
    {
      // Cannot find the name
      name = "<unknown>";
    }

    if (*information.what == 'C')
    {
      length = snprintf(buffer, size, "#%d  %s %s\n", level, name, information.short_src);

      buffer += length;
      size   -= length;

      continue;
    }

    length = snprintf(buffer, size, "#%d  %s (%s:%d)\n", level, name, information.short_src, information.currentline);

    buffer += length;
    size   -= length;
  }

  return level;
}

int MakeLuaTraceReport(siginfo_t* information, LuaTraceReportFunction report)
{
  lua_State* state;
  char buffer[BUFFER_LENGTH];

  if (state = GetLuaStateOnStack())
  {
    GetLuaTraceBack(state, buffer, BUFFER_LENGTH);
    report(LOG_ERR, "Lua stack trace:\n%s\n", buffer);
  }

  return 1;
}