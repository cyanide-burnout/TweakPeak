#ifndef CXXABITOOLS_H
#define CXXABITOOLS_H

#ifdef __cplusplus

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <typeinfo>
#include <exception>

//  CheckExceptionHandler

#define HasExceptionHandler(type)  CheckExceptionHandler(typeid(type), sizeof(type))

bool CheckExceptionHandler(const std::type_info& type, std::size_t size) noexcept;

// ExceptionTrace

struct ExceptionTrace
{
  void** begin;
  void** end;
};

extern std::atomic<unsigned> ExceptionTraceDepth;

extern "C" const ExceptionTrace* GetExceptionTrace(const void* pointer) noexcept;

// GetVirtualClassType, ...

const std::type_info* GetVirtualClassType(const void* pointer) noexcept;

extern "C" char* GetDemangledName(const char* name) noexcept;
extern "C" const char* GetVirtualClassName(const void* pointer) noexcept;

#else

char* GetDemangledName(const char* name);
const char* GetVirtualClassName(const void* pointer);

#endif

#endif
