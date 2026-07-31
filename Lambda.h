#ifndef LAMBDA_H
#define LAMBDA_H

/*
  Lambda: pass a C++ lambda into C code and call it there directly,
  with no wrapper at the call site.

  The C++ side sees std::function<type (types)>, the C side sees a
  pointer to a structure that mirrors the libstdc++ layout of
  std::function: _Any_data (two pointers), _M_manager, _M_invoker.

  This relies on:

  1. The internal layout of libstdc++'s std::function
     (pinned by the static_assert block below).

  2. The internal invoker convention _Res (*)(const _Any_data&,
     _ArgTypes&&...). Under the Itanium C++ ABI a reference is passed
     as a pointer, so the generated C prototype takes a pointer per
     argument and CallLambda() passes addresses of temporary copies
     (pinned by the static_assert block below).

  3. The Itanium C++ ABI rule that a class which is non-trivial for
     the purposes of calls is passed by value as a pointer to a
     caller-owned temporary destroyed by the caller after the call
     returns. This is why the C side declares a structure pointer
     where the C++ side declares std::function by value.

  Any of these may change with another standard library or a future
  libstdc++ release. The checks are intentionally written against
  internal libstdc++ names so that such a change stops the build
  instead of corrupting the stack.

  Rules:

  - Shared prototypes must be declared with C linkage (extern "C").

  - Signatures take C-compatible types only, by value, no parameter
    names. No array parameters, declare function-pointer parameters
    through a typedef. Arguments at C call sites must match the
    declared types exactly: the generated prototype makes the
    compiler reject a mismatch, cast explicitly where needed
    (CallLambda(handler, (const char*)"text")).

  - C code may not copy, destroy or store the object. The pointer is
    valid only while the owning C++ std::function is alive: for an
    argument created from a temporary lambda that means the duration
    of the call which received it.

  Usage:

  Common.h:

    #include "Lambda.h"

    typedef Lambda(int, int, int) HandlerFunction;

    #ifdef __cplusplus
    extern "C" {
    #endif

    void Process(HandlerFunction handler);

    #ifdef __cplusplus
    }
    #endif

  Handler.c:

    void Process(HandlerFunction handler)
    {
      if (CheckLambda(handler))
        printf("result: %d\n", CallLambda(handler, 4, 5));
    }

  Caller.cpp:

    int c = 7;
    Process([c] (int a, int b) -> int { return a + b + c; });
*/

#ifdef __cplusplus

#include <functional>
#include <type_traits>

#ifndef __GLIBCXX__
#error "Lambda.h relies on the libstdc++ implementation of std::function"
#endif

#define Lambda(type, ...)  std::function<type (__VA_ARGS__)>

static_assert(sizeof(std::function<void ()>) == (4 * sizeof(void*)),
  "libstdc++ std::function layout does not match Lambda.h");

static_assert(std::is_same<
  decltype(&std::_Function_handler<int (int), int (*)(int)>::_M_invoke),
  int (*)(const std::_Any_data&, int&&)>::value,
  "libstdc++ std::function invoker convention does not match Lambda.h");

#else

#include <stddef.h>

#define LAMBDA_TYPE(type)    , type*
#define LAMBDA_VALUE(value)  , &(__typeof__(value)){ (value) }

#define LAMBDA_MAP_0(macro)
#define LAMBDA_MAP_1(macro, item)       macro(item)
#define LAMBDA_MAP_2(macro, item, ...)  macro(item) LAMBDA_MAP_1(macro, __VA_ARGS__)
#define LAMBDA_MAP_3(macro, item, ...)  macro(item) LAMBDA_MAP_2(macro, __VA_ARGS__)
#define LAMBDA_MAP_4(macro, item, ...)  macro(item) LAMBDA_MAP_3(macro, __VA_ARGS__)
#define LAMBDA_MAP_5(macro, item, ...)  macro(item) LAMBDA_MAP_4(macro, __VA_ARGS__)
#define LAMBDA_MAP_6(macro, item, ...)  macro(item) LAMBDA_MAP_5(macro, __VA_ARGS__)
#define LAMBDA_MAP_7(macro, item, ...)  macro(item) LAMBDA_MAP_6(macro, __VA_ARGS__)
#define LAMBDA_MAP_8(macro, item, ...)  macro(item) LAMBDA_MAP_7(macro, __VA_ARGS__)

#define LAMBDA_COUNT(...)  LAMBDA_SELECT(_, ##__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LAMBDA_SELECT(_0, _1, _2, _3, _4, _5, _6, _7, _8, count, ...)  count

#define LAMBDA_CONCAT(name, count)  name##count
#define LAMBDA_INVOKE(name, count)  LAMBDA_CONCAT(name, count)
#define LAMBDA_MAP(macro, ...)      LAMBDA_INVOKE(LAMBDA_MAP_, LAMBDA_COUNT(__VA_ARGS__))(macro, ##__VA_ARGS__)

#define Lambda(type, ...)                                                          \
    struct                                                                         \
    {                                                                              \
      char data[2 * sizeof(void*)];                                                \
      void* manager;                                                               \
      type (*invoker)(const void* functor LAMBDA_MAP(LAMBDA_TYPE, ##__VA_ARGS__)); \
    } *

#define CallLambda(lambda, ...)                                                           \
    ({                                                                                    \
      __auto_type LambdaObject = (lambda);                                                \
      LambdaObject->invoker(&LambdaObject->data LAMBDA_MAP(LAMBDA_VALUE, ##__VA_ARGS__)); \
    })

#define CheckLambda(lambda)                                      \
    ({                                                           \
      __auto_type LambdaObject = (lambda);                       \
      (LambdaObject != NULL) && (LambdaObject->manager != NULL); \
    })

#endif

#endif
