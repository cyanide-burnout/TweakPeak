# TweakPeak

Extracts of various runtime tweaks I use in my projects for a long time

## CXXABITools

*Note!* this component requires GNU libc header https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/unwind-cxx.h of (almost) exact version as runtime has. Code tested with clang++ and g++ on arm64 and amd64 architectures on Debian 12.

### HasExceptionHandler(type)

What if you need to check at some point for existance of proper exception handler?

```C++
  try
  {
    printf("std::runtime_error result=%d\n", HasExceptionHandler(std::runtime_error));
    printf("std::exception     result=%d\n", HasExceptionHandler(std::exception));
    printf("const char*        result=%d\n", HasExceptionHandler(const char*));
    printf("int                result=%d\n", HasExceptionHandler(int));
  }
  catch (const std::exception& exception)  {  }
  catch (const char* exception)            {  }

```

### ExceptionTrace

When you need to get a stack trace of thrown exception. Well, ExceptionTrace doesn't provide text representation of stack trace, but a copy of instruction pointers to recover or check calls. You can generate printable form when you need by using for example dladdr().


```C++
  
  ExceptionTraceDepth = 16;  // This one is required to enable backtraces, value is the trace depth

  try
  {
    throw std::runtime_error("test");
  }
  catch (const std::exception& exception)
  {
    const ExceptionTrace* trace;
    Dl_info information;

    printf("exception: %s\n", exception.what());

    if (trace = GetExceptionTrace(&exception))
    {
      for (void** entry = trace->begin; entry != trace->end; entry ++)
      {
        dladdr(*entry, &information);
        printf("frame: %p %s %s\n", *entry, information.dli_sname, information.dli_fname);
          continue;
      }
    }
  }
```

### GetVirtualClassType

This call is useful when you need to get exact class type from pointer and you completely sure it has vtable.

- const std::type_info* GetVirtualClassType(const void* pointer)

## LuaTrace

When you need to get a trace from Lua virtual machine (luajit or liblua) at least on 64-bit architectures (SysV ABI).
Compolent requires to be include to your lua caller to enable tracing.

- lua_State* GetLuaStateOnStack() - returns the last instance of lua_State on the stack
- int GetLuaTraceBack(lua_State* state, char* buffer, size_t size) - get printable form of Lua's stack trace

## WatchPoint

Useful when you want to install breakpoints on conditional manner.
Please note, that you have to handle SIGTRAP in your own code. WatchPoint installs child process as a debugger for your process and manages addresses to watch.
When break condition reached, SIGTRAP will be called on the main process, number of is available by call GetWatchPoint().

- SetWatchPoint(int number, const void* address, uint32_t condition) - installs / modifies watch point
- GetWatchPoint() - returns last triggered watch point

```C++
static void HandleFaultSignal(int signal, siginfo_t* information, void* context)
{
  if ((information->si_signo == SIGTRAP) &&
      (information->si_code  == TRAP_HWBKPT))
  {
    syslog(LOG_ERR, "The process has been trapped by Watch Point %i\n", GetWatchPoint());
    // ... log backtrace here ...
  }
}

SetWatchPoint(0, &something, WATCHPOINT_BREAK_ON_WRITE | WATCHPOINT_LENGTH_DWORD);

```

## P.S.

Maybe later I'll disclose much more tools I've developed in my personal projects.
