#ifndef DEBUGDECODER_H
#define DEBUGDECODER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>

#include <link.h>
#include <dlfcn.h>
#include <libdwarf/libdwarf.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DEBUG_UPDATE_SYNCHRONOUS   0
#define DEBUG_UPDATE_ASYNCHRONOUS  1

#define DEBUG_GET_LOCK_WAIT       0
#define DEBUG_GET_LOCK_DONT_WAIT  1

struct DebugSourceInformation
{
  char* path;
  uintptr_t address;
  Dwarf_Unsigned line;
  Dwarf_Unsigned column;
  Dwarf_Debug instance;
};

int GetDebugInformation(Dl_info* information, struct link_map* map, uintptr_t address, struct DebugSourceInformation* buffer, int lock);
void ReleaseDebugInformation(struct DebugSourceInformation* information);

void UpdateDebugCache(int option);
void CancelUpdateDebugCache();

#ifdef __cplusplus
}
#endif

#endif
