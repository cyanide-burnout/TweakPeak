#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <unwind.h>
#include <cxxabi.h>
#include <dlfcn.h>

#include <alloca.h>
#include <malloc.h>
#include <string.h>

#include <numeric>
#include <iterator>

// https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/unwind-cxx.h
#undef _GLIBCXX_HAVE_SYS_SDT_H
#include "Import/unwind-cxx.h"

#if !defined(__arm__) && !defined(__aarch64__)

// By default libstdc uses libunwind wrapper:
//   https://github.com/gcc-mirror/gcc/blob/master/libgcc/unwind-compat.c
//   https://github.com/libunwind/libunwind/tree/master/src/unwind
// But on several architectures such as ARM it uses custom implementations:
//   https://github.com/gcc-mirror/gcc/blob/master/gcc/ginclude/unwind-arm-common.h

#define USE_GENERIC_UNWIND
#endif

#include "CXXABITools.h"

// CheckExceptionHandler

// https://maskray.me/blog/2020-12-12-c++-exception-handling-abi
// https://blog.the-pans.com/cpp-exception-3/
// https://monoinfinito.wordpress.com/series/exception-handling-in-c/
// https://habr.com/ru/post/279149/
// https://stackoverflow.com/questions/15670169/what-is-difference-between-sjlj-vs-dwarf-vs-seh

union AlignedExeptionPointer
{
  void* address;
  uintptr_t alignment;
  __cxxabiv1::__cxa_refcounted_exception* exception;
};

static bool CheckExceptionHandler(void* context, _Unwind_Exception* exception, int skip) noexcept
{
  unw_cursor_t cursor;
  unw_proc_info_t information;

  _Unwind_Context* state;
  _Unwind_Stop_Fn function;
  _Unwind_Reason_Code reason;

#ifndef USE_GENERIC_UNWIND

  int stack;
  int region;
  int language;
  int instruction;
  unw_word_t buffer[512];

  // Since not every runtime uses libunwind, the following part
  // is required to recover a structure _Unwind_Context

  state = reinterpret_cast<_Unwind_Context*>(buffer);

  std::iota(std::begin(buffer), std::end(buffer), 0);

  instruction = _Unwind_GetIP(state);
  stack       = _Unwind_GetCFA(state);
  region      = _Unwind_GetRegionStart(state);
  language    = reinterpret_cast<uintptr_t>(_Unwind_GetLanguageSpecificData(state));

  memset(buffer, 0, sizeof(buffer));

#else

  state = reinterpret_cast<_Unwind_Context*>(&cursor);

#endif

  // Unwind stack, call every personality for search

  if (context != nullptr)
  {
    // Likely called by signal handler with alternative stack
    unw_init_local2(&cursor, reinterpret_cast<unw_context_t*>(context), UNW_INIT_SIGNAL_FRAME);
  }
  else
  {
    context = alloca(sizeof(unw_context_t));
    unw_getcontext(reinterpret_cast<unw_context_t*>(context));
    unw_init_local(&cursor, reinterpret_cast<unw_context_t*>(context));
  }

  while ((skip > 0) &&
         (unw_step(&cursor) > 0))
  {
    // It seems like noexcept functions make false positives
    skip --;
  }

  while (unw_step(&cursor) > 0)
  {
    if ((unw_get_proc_info(&cursor, &information) == UNW_ESUCCESS) &&
        (function = reinterpret_cast<_Unwind_Stop_Fn>(information.handler)))
    {
#ifndef USE_GENERIC_UNWIND
      buffer[region]   = information.start_ip;
      buffer[language] = information.lsda;

      unw_get_reg(&cursor, UNW_REG_IP, buffer + instruction);
      unw_get_reg(&cursor, UNW_REG_SP, buffer + stack);
#endif

      reason = function(1, _UA_SEARCH_PHASE, exception->exception_class, exception, state, nullptr);

      if (reason == _URC_HANDLER_FOUND)    return true;
      if (reason != _URC_CONTINUE_UNWIND)  return false;
    }
  }

  return false;
}

bool CheckExceptionHandler(void* context, const std::type_info& type, std::size_t size) noexcept
{
  AlignedExeptionPointer pointer;

  size += sizeof(__cxxabiv1::__cxa_refcounted_exception);

  pointer.address    = alloca(size + __BIGGEST_ALIGNMENT__);
  pointer.alignment +=  (__BIGGEST_ALIGNMENT__ - 1ULL);
  pointer.alignment &= ~(__BIGGEST_ALIGNMENT__ - 1ULL);

  memset(pointer.exception, 0, size);
  __cxxabiv1::__cxa_init_primary_exception(pointer.exception + 1, const_cast<std::type_info*>(&type), nullptr);

  return CheckExceptionHandler(context, &pointer.exception->exc.unwindHeader, 2);
}

// ExceptionTrace

// https://github.com/gcc-mirror/gcc/blob/master/libstdc++-v3/libsupc++/eh_alloc.cc
// https://github.com/boostorg/stacktrace/blob/develop/src/from_exception.cpp

#define EXCEPTION_TRACE_MAGIC  0xaec5b15b7c84baeeULL

struct TraceableException
{
  struct ExceptionTrace trace;
  uint64_t alignment;
  uint64_t magic;
  __cxxabiv1::__cxa_refcounted_exception exception;
  char data[0];
};

typedef void* (*AllocateExceptionFunction)(std::size_t size) noexcept;
typedef void (*FreeExceptionFunction)(void* pointer) noexcept;

static AllocateExceptionFunction AllocateException = nullptr;
static FreeExceptionFunction     FreeException     = nullptr;

std::atomic<unsigned> ExceptionTraceDepth(0);

static void __attribute__((constructor(103))) Initialize()
{
  AllocateException = reinterpret_cast<AllocateExceptionFunction>(dlsym(RTLD_NEXT, "__cxa_allocate_exception"));
  FreeException     = reinterpret_cast<  FreeExceptionFunction  >(dlsym(RTLD_NEXT, "__cxa_free_exception"));
}

extern "C" void* __cxxabiv1::__cxa_allocate_exception(std::size_t size) noexcept
{
  unsigned depth;
  unw_word_t address;
  unw_cursor_t cursor;
  unw_context_t context;
  TraceableException* exception;

  depth     = ExceptionTraceDepth.load(std::memory_order_relaxed);
  exception = static_cast<TraceableException*>(malloc(sizeof(TraceableException) + size + depth * sizeof(void*) + __BIGGEST_ALIGNMENT__));

  if (exception != nullptr)
  {
    size +=  (__BIGGEST_ALIGNMENT__ - 1ULL);
    size &= ~(__BIGGEST_ALIGNMENT__ - 1ULL);

    exception->magic       = EXCEPTION_TRACE_MAGIC;
    exception->trace.begin = reinterpret_cast<void**>(exception->data + size);
    exception->trace.end   = exception->trace.begin;

    memset(&exception->exception, 0, sizeof(__cxxabiv1::__cxa_refcounted_exception));

    if (depth != 0)
    {
      unw_getcontext(&context);
      unw_init_local(&cursor, &context);

      while ((depth != 0) &&
             (unw_step(&cursor) > 0))
      {
        if (unw_get_reg(&cursor, UNW_REG_IP, &address) == UNW_ESUCCESS)
        {
          // Copy resolved IPs only
          *(exception->trace.end ++) = reinterpret_cast<void*>(address);
        }

        depth --;
      }
    }

    return exception->data;
  }

  return AllocateException(size);
}

extern "C" void __cxxabiv1::__cxa_free_exception(void* pointer) noexcept
{
  TraceableException* exception;

  exception = static_cast<TraceableException*>(pointer) - 1;

  if ((exception->magic       == EXCEPTION_TRACE_MAGIC) &&
      (exception->trace.begin <= exception->trace.end))
  {
    free(exception);
    return;
  }

  FreeException(pointer);
}

extern "C" const ExceptionTrace* GetExceptionTrace(const void* pointer) noexcept
{
  const TraceableException* exception;

  exception = static_cast<const TraceableException*>(pointer) - 1;

  return
    (exception->magic       == EXCEPTION_TRACE_MAGIC) &&
    (exception->trace.begin <= exception->trace.end)  ?
    &exception->trace                                 : 
    nullptr;
}

// GetVirtualClassType / GetDemangledName

const std::type_info* GetVirtualClassType(const void* pointer) noexcept
{
  const uintptr_t* object;
  const uintptr_t* table;

  // https://guihao-liang.github.io/2020/05/30/what-is-vtable-in-cpp

  if (pointer != nullptr)
  {
    object = reinterpret_cast<const uintptr_t*>(pointer);
    table  = reinterpret_cast<const uintptr_t*>(object[0]);     // In case of virtual class it always begins with a pointer to vtable
    return reinterpret_cast<const std::type_info*>(table[-1]);  // Actual vtable has two pointers prefix: 0 and std::type_info*
  }

  return nullptr;
}

char* GetDemangledName(const char* name) noexcept
{
  int status;

  return abi::__cxa_demangle(name, nullptr, nullptr, &status);
}

const char* GetVirtualClassName(const void* pointer) noexcept
{
  const char* name;
  const std::type_info* type;

  type = GetVirtualClassType(pointer);

  if (type != nullptr)
  {
    for (name = type->name(); (*name >= '0') && (*name <= '9'); ++ name);
    return name;
  }

  return nullptr;
}
