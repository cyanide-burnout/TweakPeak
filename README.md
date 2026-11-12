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

```C
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

## DebugDecoder

Allows to resolve source file and line from instruction pointer using DWARF.

- Could be used in different cases such as stack backtrace
- Could load DWARF from binary
- In case of stripped binary:
  - tries to load DWARF from /usr/lib/debug/ (usually used by debug symbol packages)
  - tries to load DWARF using libdebuginfod (https://sourceware.org/elfutils/Debuginfod.html)
- Has caching.
- Allows loading on-demand as well as synchronous and asynchronous preload.

### Preload

Preload is optional.

- UpdateDebugCache(DEBUG_UPDATE_SYNCHRONOUS)
- UpdateDebugCache(DEBUG_UPDATE_ASYNCHRONOUS)
- CancelUpdateDebugCache()

### GetDebugInformation

int GetDebugInformation(Dl_info* information, struct link_map* map, uintptr_t address, struct DebugSourceInformation* buffer, int lock)

- *information* and *map* are optioan and required only to save some CPU cycles if you need to call dladdr1() before
- *address* is an instruction pointer value
- *lock* depends on your need:
  - DEBUG_GET_LOCK_WAIT - get data anyway, but deadlock might happen (useful in regular code)
  - DEBUG_GET_LOCK_DONT_WAIT - avoid a deadlock, data should not be provided when locked (usuful in signal handlers)

### Usage

Without reuse dladdr1() data:


```C
  struct DebugSourceInformation result;

  uint64_t address = (uint64_t)(void*)HandleProgramHeader1;

  if (GetDebugInformation(NULL, NULL, address, &result, DEBUG_GET_LOCK_DONT_WAIT) != 0)
  {
    printf("result: %s:%d.%d %d\n", result.path, result.line, result.column, result.address == address);
    ReleaseDebugInformation(&result);
  }

```

With reuse dladdr1() data:

```C
  Dl_info information;
  struct link_map* map;

  if (dladdr1(address, &information, (void**)&map, RTLD_DL_LINKMAP) != 0)
  {
    if (GetDebugInformation(&information, map, (uintptr_t)address, &source, DEBUG_GET_LOCK_DONT_WAIT) != 0)
    {
      printf("%s (%s) - %s:%lld.%lld\n", information.dli_sname, information.dli_fname, source.path, source.line, source.column);
      ReleaseDebugInformation(&source);
    }
    else
    {
      printf("%s (%s)\n", information.dli_sname, information.dli_fname);      
    }
  }

```

## Lambda

Allows passing a capturing C++ lambda to C code and invoking it there directly, without a wrapper at the call site.

This header-only bridge mirrors the internal layout, invoker and manager conventions of `std::function`. It requires GNU libstdc++ and the Itanium C++ ABI, as used by amd64 and arm64 Linux. Shared signatures may contain only C-compatible types passed by value and are limited to eight arguments.

### Compatible callables

Lambdas are the primary use case, but the C++ parameter is a `std::function`. Any compatible callable accepted by `std::function` therefore crosses through the same C representation and API without a dedicated adapter. This includes function pointers, stateful functors, `std::bind`/`std::bind_front` results and `std::reference_wrapper`.

The conversion happens on the C++ side. C always receives the same erased object and calls it through `InvokeLambda()`, regardless of the original callable type.

C code may invoke the object with `InvokeLambda()` and check whether it is empty with `CheckLambda()`. The received pointer is valid only while the owning C++ `std::function` is alive.

`LambdaHolder(type)` is owning storage represented by a native `std::function` in C++ and its inline layout mirror in C. It can be declared in shared headers and embedded in shared structures, but must cross the language boundary only by pointer, never by value or `memcpy`. C storage must be zero-initialized and explicitly destroyed; C++ storage has normal `std::function` lifetime and may be manipulated from C.

Shared header:

```C
#include "Lambda.h"

typedef Lambda(int, int) Handler;

#ifdef __cplusplus
extern "C" {
#endif

int Process(Handler handler);

#ifdef __cplusplus
}
#endif
```

C implementation:

```C
int Process(Handler handler)
{
  return CheckLambda(handler) ? InvokeLambda(handler, 5) : -1;
}
```

C++ caller:

```C++
int offset = 7;
int result = Process([offset] (int value) { return value + offset; });
```

### Storing

To retain a lambda beyond the call in C, declare zero-initialized storage with `LambdaHolder()`, copy the closure with `CloneLambda()` and release it with `DestroyLambda()`.

`CloneLambda()` replaces any previous content of the target and also supports cloning a holder over itself. `DestroyLambda()` leaves the holder empty and may be called repeatedly. Cloning invokes the capture's copy constructor and may allocate; an exception propagating through C code should be treated as fatal.

```C
static LambdaHolder(Handler) Stored;

void Store(Handler handler)
{
  CloneLambda(&Stored, handler);
}

int InvokeStored(int value)
{
  return CheckLambda(&Stored) ? InvokeLambda(&Stored, value) : -1;
}

void ReleaseStored(void)
{
  DestroyLambda(&Stored);
}
```

## Selector

When you can't get away from `select()` but have many handles registered and only a few ready at a time. Selector walks the ready bitmaps returned by the kernel instead of the list of registered handlers: each 64-bit word is checked for zero, set bits are extracted with `ctz`, and the handler is found by direct array index. User-space cost of one pass is O(size / 64 + ready) instead of O(registered).

Storage grows on demand (doubling) when a handle beyond the current size is registered, also from a handler during a dispatch, and never shrinks: a handle number reached once will be reached again. The whole storage is passed to `select()` as `nfds`, an empty tail costs only a few zero words. Size may exceed `FD_SETSIZE`. Linux accepts larger sets natively, on Darwin `select()` returns `EINVAL` for `nfds > FD_SETSIZE` unless `_DARWIN_UNLIMITED_SELECT` is defined, which links `select$DARWIN_EXTSN` instead. Since `select()` is called by your code, the macro has to be in effect in your translation unit: Selector.h defines it, so include it before anything that pulls in `<sys/select.h>`, or pass `-D_DARWIN_UNLIMITED_SELECT` project wide. The practical ceiling is then `RLIMIT_NOFILE`, bounded by `kern.maxfilesperproc`; note that processes started by launchd get a soft limit of 256 and have to raise it with `setrlimit()`. Registration masks are kept persistent and copied with `memcpy()` before every call, no `FD_SET()` per handle. The code relies on the little-endian bit layout of `fd_set` shared by glibc and Darwin, the build fails on other byte orders.

Dispatch is reentrant: handlers may add, change or remove any handle, including their own and the ones still pending in the current pass. Removing an event also drops it from the pending results, so a handler is never called for an event it is no longer registered for, and a handle number closed and reused by a new owner within the same pass never receives the events of the old one. The only rule is to remove a handle (events = 0) before closing it. Calling `PrepareSelector()` or `DispatchSelector()` from a handler fails with `EDEADLK`.

The wait itself stays with the caller: `PrepareSelector()` fills the sets and returns `nfds`, you call `select()` or `pselect()` with your own timeout and signal mask, and pass its result to `DispatchSelector()`. The sets stay valid until a registration grows the storage, so don't register anything between `PrepareSelector()` and the return of `select()`.

```C
static void HandleRead(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  char buffer[256];

  if (read(handle, buffer, sizeof(buffer)) <= 0)
  {
    SetSelectorHandler(selector, handle, 0, NULL, NULL);  // remove before close
    close(handle);
  }
}

struct Selector* selector = CreateSelector();

SetSelectorHandler(selector, STDIN_FILENO, SELECTOR_EVENT_READ, HandleRead, NULL);

fd_set* readable;
fd_set* writable;
fd_set* exceptional;
int count;

do
{
  count = PrepareSelector(selector, &readable, &writable, &exceptional);
  count = select(count, readable, writable, exceptional, NULL);
  count = DispatchSelector(selector, count);
}
while ((count >= 0) || (errno == EINTR));

ReleaseSelector(selector);
```

### Tests and benchmark

`SelectorTest.c` covers the reentrancy cases: a pending handle closed and its number reused within the same pass, dropping one event of a pending handle, storage growth from a handler, handles above `FD_SETSIZE`, nested calls and argument checks.

```sh
cc -std=c11 -g -fsanitize=address,undefined Selector.c SelectorTest.c -o SelectorTest && ./SelectorTest
cc -std=c11 -O2 Selector.c SelectorBench.c -o SelectorBench && ./SelectorBench [simulated | real]
```

`SelectorBench.c` compares Selector with the classic loop over a list of individually allocated source objects (`FD_ZERO` + `FD_SET` per source, then `FD_ISSET` per source). The `simulated` mode replaces `select()` with copying a prepared result to measure the user-space part alone, the `real` mode runs `select()` with a zero timeout over pipes.

Apple M3 Max, ns per iteration, N registered, R ready:

| N | R | classic | Selector |
|---|---|---|---|
| 64 | 1 | 227 | 16 |
| 64 | 64 | 257 | 289 |
| 1024 | 1 | 3595 | 32 |
| 1024 | 102 | 4309 | 203 |
| 1024 | 1024 | 5249 | 2702 |
| 4096 | 1 | 36675 | 110 |

With few ready handles the user-space cost barely depends on N. With all of them ready and small N, Selector is slower than a loop with a persistent mask: reentrancy makes it reload the sets after every handler call. With the real `select()` the kernel dominates (about 30 µs for 1024 pipes), so the end-to-end gain is about 1.0–1.4×.


## P.S.

Maybe later I'll disclose much more tools I've developed in my personal projects.
