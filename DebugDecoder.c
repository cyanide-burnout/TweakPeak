#include "DebugDecoder.h"

// https://github.com/bombela/backward-cpp/blob/26b244db5509d589b316d2ac4a9ffb805a74786e/backward.hpp#L2104
// https://github.com/avast/libdwarf/blob/master/libdwarf/dwarfexample/simplereader.c
// https://www.prevanders.net/libdwarfdoc/index.html

#include <stdatomic.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <link.h>

#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gelf.h>
#include <libelf.h>
#include <libdwarf/dwarf.h>
#include <elfutils/debuginfod.h>

#include <stdlib.h>
#include <stdio.h>

struct SourceLine
{
  Dwarf_Addr address;
  Dwarf_Line* line;
};

struct SourceCache
{
  struct SourceCache* next;

  Dwarf_Die entry;
  Dwarf_Off offset;
  Dwarf_Small count;
  Dwarf_Line_Context context;

  Dwarf_Signed length;
  struct SourceLine* list;
};

struct DebugUnit
{
  char* name;
  uintptr_t next;
  pthread_mutex_t lock;

  int handle;                   // |
  Elf* module;                  // | Debug unit cache
  Dwarf_Debug instance;         // |

  Dwarf_Signed count;           // Aranges cache
  Dwarf_Arange* aranges;        //   -- // --

  struct SourceCache* sources;  // Source cache
};

struct NameList
{
  char* data;
  size_t size;
  size_t length;
};

static atomic_int state;
static atomic_uintptr_t cache;
static debuginfod_client* client;

// ELF helper

static Elf_Scn* GetELFSection(Elf* image, const char* goal, GElf_Shdr* header)
{
  GElf_Shdr* pointer;
  Elf_Scn* section;
  size_t index;
  char* name;
  int number;

  if (elf_getshdrstrndx(image, &index) >= 0)
  {
    number = 0;
    while (section = elf_getscn(image, ++ number))
    {
      if ((gelf_getshdr(section, header) != NULL) &&
          (name = elf_strptr(image, index, header->sh_name)) &&
          (strcmp(name, goal) == 0))
      {
        // Buffer for GElf_Shdr has allways to be passed
        return section;
      }
    }
  }

  return NULL;
}

static uint8_t* GetBuildID(Elf* image, size_t* size)
{
  GElf_Shdr header1;
  GElf_Nhdr header2;
  Elf_Scn* section;
  Elf_Data *data;
  size_t offset1;
  size_t offset2;
  size_t offset3;
  size_t length;

  if ((section = GetELFSection(image, ".note.gnu.build-id", &header1)) &&
      (data    = elf_getdata(section, NULL)))
  {
    offset1 = 0;
    while (length = gelf_getnote(data, offset1, &header2, &offset2, &offset3))
    {
      offset1 += length;

      if ((header2.n_type == NT_GNU_BUILD_ID) &&
          (strcmp(data->d_buf + offset2, ELF_NOTE_GNU) == 0))
      {
        *size = header2.n_descsz;
        return data->d_buf + offset3;
      }
    }
  }

  return NULL;
}

// Load and cache

static void ReleaseDebugUnitCache()
{
  Dwarf_Error error;
  struct DebugUnit* unit;
  struct DebugUnit* next;
  struct SourceCache* source;

  unit = (struct DebugUnit*)atomic_exchange_explicit(&cache, 0, memory_order_relaxed);

  while (unit != NULL)
  {
    next = (struct DebugUnit*)unit->next;

    while (unit->sources != NULL)
    {
      source        = unit->sources;
      unit->sources = source->next;

      dwarf_srclines_dealloc_b(source->context);
      dwarf_dealloc(unit->instance, source->entry, DW_DLA_DIE);
      free(source->list);
      free(source);
    }

    if (unit->aranges != NULL)
    {
      while (unit->count > 0)
      {
        unit->count --;
        dwarf_dealloc(unit->instance, unit->aranges[unit->count], DW_DLA_ARANGE);
      }

      dwarf_dealloc(unit->instance, unit->aranges, DW_DLA_LIST);
    }

    dwarf_finish(unit->instance, &error);
    elf_end(unit->module);
    close(unit->handle);

    pthread_mutex_destroy(&unit->lock);
    free(unit->name);
    free(unit);

    unit = next;
  }
}

static void __attribute__((constructor(103))) Initialize()
{
  atomic_init(&cache, 0);
  elf_version(EV_CURRENT);
  client = debuginfod_begin();
}

static void __attribute__((destructor)) Finalize()
{
  ReleaseDebugUnitCache();
  debuginfod_end(client);
}

static struct DebugUnit* GetDebugUnit(const char* name, debuginfod_client* client)
{
  struct DebugUnit* unit;

  int result;
  size_t size;
  struct stat status;
  char path[PATH_MAX];

  GElf_Shdr header;
  Dwarf_Error error;
  uint8_t* identifier;

  // Try to find a unit in the cache

  for (unit = (struct DebugUnit*)atomic_load_explicit(&cache, memory_order_relaxed);
    (unit != NULL) && (strcmp(name, unit->name) != 0); unit = (struct DebugUnit*)unit->next);

  // Create a new unit otherwise

  if ((unit == NULL) &&
      (unit       = (struct DebugUnit*)calloc(1, sizeof(struct DebugUnit))) &&
      (unit->name = strdup(name)))
  {
    pthread_mutex_init(&unit->lock, NULL);

    // Try to load unit directly from the binary

    unit->handle = open(name, O_RDONLY);

    if ((unit->handle >= 0) &&
        (unit->module  = elf_begin(unit->handle, ELF_C_READ_MMAP, NULL)) &&
        (GetELFSection(unit->module, ".debug_info", &header) != NULL))
    {
      // Result does not matter
      dwarf_elf_init(unit->module, DW_DLC_READ, NULL, NULL, &unit->instance, &error);
    }

    // Try to load separated .debug file
    // https://sourceware.org/gdb/onlinedocs/gdb/Separate-Debug-Files.html

    if ((unit->instance == NULL) &&
        (unit->module   != NULL) &&
        (identifier = GetBuildID(unit->module, &size)) &&
        (size == 20) &&
        (sprintf(path, "/usr/lib/debug/.build-id/%02x/%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.debug",
          identifier[ 0], identifier[ 1], identifier[ 2], identifier[ 3], identifier[ 4], identifier[ 5], identifier[ 6], identifier[ 7], identifier[ 8], identifier[ 9],
          identifier[10], identifier[11], identifier[12], identifier[13], identifier[14], identifier[15], identifier[16], identifier[17], identifier[18], identifier[19]) > 0) &&
        (stat(path, &status) == 0))
    {
      elf_end(unit->module);
      close(unit->handle);

      unit->handle = open(path, O_RDONLY);
      unit->module = NULL;

      if ((unit->handle >= 0) &&
          (unit->module  = elf_begin(unit->handle, ELF_C_READ_MMAP, NULL)))
      {
        // Result does not matter
        dwarf_elf_init(unit->module, DW_DLC_READ, NULL, NULL, &unit->instance, &error);
      }
    }

    // Try to load file from debuginfod
    // https://sourceware.org/elfutils/Debuginfod.html

    if ((unit->instance == NULL) &&
        (unit->module   != NULL) &&
        (client     != NULL) &&
        (identifier != NULL) &&
        ((result = debuginfod_find_debuginfo(client, identifier, size, NULL)) >= 0))
    {
      elf_end(unit->module);
      close(unit->handle);

      unit->handle = result;
      unit->module = NULL;

      if (unit->module = elf_begin(unit->handle, ELF_C_READ_MMAP, NULL))
      {
        // Result does not matter
        dwarf_elf_init(unit->module, DW_DLC_READ, NULL, NULL, &unit->instance, &error);
      }
    }

    // Clean if failed

    if (unit->instance == NULL)
    {
      elf_end(unit->module);
      close(unit->handle);

      unit->module = NULL;
      unit->handle = -1;
    }

    // Add unit to the cache

    do { unit->next = atomic_load_explicit(&cache, memory_order_relaxed); }
    while (!atomic_compare_exchange_strong_explicit(&cache, &unit->next, (uintptr_t)unit, memory_order_release, memory_order_relaxed));
  }

  return unit;
}

// Search routines

static int CompareAddresses(const void* pointer1, const void* pointer2)
{
  struct SourceLine* line1;
  struct SourceLine* line2;

  line1 = (struct SourceLine*)pointer1;
  line2 = (struct SourceLine*)pointer2;

  return (line1->address > line2->address) - (line1->address < line2->address);
}

static int CheckRange(struct DebugUnit* unit, Dwarf_Die entry, uintptr_t address)
{
  Dwarf_Half form;
  Dwarf_Error error;
  Dwarf_Unsigned low;
  Dwarf_Unsigned high;
  enum Dwarf_Form_Class type;

  Dwarf_Off offset;
  Dwarf_Signed count;
  Dwarf_Ranges* ranges;
  Dwarf_Unsigned length;
  Dwarf_Attribute attribute;

  int number;
  int result;

  low    = 0;
  high   = 0;
  result = 0;
  offset = 0;

  if ((dwarf_lowpc(entry, &low, &error)                   == DW_DLV_OK) &&
      (dwarf_highpc_b(entry, &high, &form, &type, &error) == DW_DLV_OK))
  {
    high += low * (type == DW_FORM_CLASS_CONSTANT);
    return (address >= low) && (address < high);
  }

  if (dwarf_attr(entry, DW_AT_ranges, &attribute, &error) == DW_DLV_OK)  // <-- Memory leak?
  {
    if ((dwarf_global_formref(attribute, &offset, &error)                                    == DW_DLV_OK) &&
        (dwarf_get_ranges_a(unit->instance, offset, entry, &ranges, &count, &length, &error) == DW_DLV_OK))
    {
      for (number = 0; number < count; ++ number)
      {
        if ((ranges[number].dwr_addr1 != 0) &&
            (address >= ranges[number].dwr_addr1 + low) &&
            (address <  ranges[number].dwr_addr2 + low))
        {
          result = 1;
          break;
        }
      }

      dwarf_ranges_dealloc(unit->instance, ranges, count);
    }

    dwarf_dealloc(unit->instance, attribute, DW_DLA_ATTR);
  }

  return result;
}

static int IterateOverChildren(struct DebugUnit* unit, Dwarf_Die* entry, uintptr_t address)
{
  int result;

  Dwarf_Die current;
  Dwarf_Die previous;

  Dwarf_Half tag;
  Dwarf_Bool flag;
  Dwarf_Error error;
  Dwarf_Attribute attribute;

  if (dwarf_child(*entry, &current, &error) == DW_DLV_OK)
  {
    do
    {
      if ((dwarf_tag(current, &tag, &error) == DW_DLV_OK) &&
          ((tag == DW_TAG_subprogram) || (tag == DW_TAG_inlined_subroutine)) &&
          (CheckRange(unit, current, address) != 0))
      {
          /*
            Don't replace entry with found DIE:
              dwarf_dealloc(unit->instance, *entry, DW_DLA_DIE);
              *entry = current;
          */
          dwarf_dealloc(unit->instance, current, DW_DLA_DIE);
          return 1;
      }

      if (dwarf_attr(current, DW_AT_declaration, &attribute, &error) == DW_DLV_OK)
      {
        flag = 0;
        dwarf_formflag(attribute, &flag, &error);
        dwarf_dealloc(unit->instance, attribute, DW_DLA_ATTR);

        if ((flag != 0) &&
            (IterateOverChildren(unit, &current, address) != 0))
        {
          /*
            Don't replace entry with found DIE:
              dwarf_dealloc(unit->instance, *entry, DW_DLA_DIE);
              *entry = current;
          */
          dwarf_dealloc(unit->instance, current, DW_DLA_DIE);
          return 1;
        }
      }

      previous = current;
      result   = dwarf_siblingof(unit->instance, previous, &current, &error);  // <-- Memory leak?

      dwarf_dealloc(unit->instance, previous, DW_DLA_DIE);
    }
    while (result == DW_DLV_OK);
  }

  return 0;
}

static Dwarf_Die GetDebugEntry(struct DebugUnit* unit, uintptr_t address)
{
  Dwarf_Die entry;

  Dwarf_Off offset;
  Dwarf_Error error;
  Dwarf_Arange arange;

  Dwarf_Half tag;
  Dwarf_Unsigned low;
  Dwarf_Unsigned high;

  entry = NULL;
  error = NULL;

  // Try to get address by aranges

  if (((unit->aranges != NULL) ||
       (dwarf_get_aranges(unit->instance, &unit->aranges, &unit->count, &error) == DW_DLV_OK)) &&
      (dwarf_get_arange(unit->aranges, unit->count, address, &arange, &error)   == DW_DLV_OK)  &&
      (dwarf_get_cu_die_offset(arange, &offset, &error)                         == DW_DLV_OK))
  {
    // Result code does not matter
    dwarf_offdie_b(unit->instance, offset, 1, &entry, &error);
  }

  // Try to find address by scanning all compilation units by a high/low PCs

  if (entry == NULL)
  {
    while (dwarf_next_cu_header_d(unit->instance, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &offset, NULL, &error) == DW_DLV_OK)
    {
      if ((entry == NULL) &&  // Don't optimize a loop, cursor has to pass all compilation units
          (dwarf_siblingof(unit->instance, NULL, &entry, &error) == DW_DLV_OK) &&
          ((dwarf_tag(entry, &tag, &error) != DW_DLV_OK) ||
           (tag != DW_TAG_compile_unit) ||
           (CheckRange(unit, entry, address) == 0)))
      {
        dwarf_dealloc(unit->instance, entry, DW_DLA_DIE);
        entry = NULL;
      }
    }
  }

  // Try again by looking at all DIEs in all compilation units

  if (entry == NULL)
  {
    while (dwarf_next_cu_header_d(unit->instance, 1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &offset, NULL, &error) == DW_DLV_OK)
    {
      if ((entry == NULL) &&  // Don't optimize a loop, cursor has to pass all compilation units
          (dwarf_siblingof(unit->instance, NULL, &entry, &error) == DW_DLV_OK) &&
          (IterateOverChildren(unit, &entry, address) == 0))
      {
        dwarf_dealloc(unit->instance, entry, DW_DLA_DIE);
        entry = NULL;
      }
    }
  }

  return entry;
}

// Code resolution

static struct SourceCache* GetSourceCache(struct DebugUnit* unit, Dwarf_Die entry)
{
  struct SourceCache* source;
  struct SourceLine* last;
  Dwarf_Unsigned version;
  Dwarf_Error error;
  Dwarf_Line* limit;
  Dwarf_Line* line;
  Dwarf_Off offset;

  // Result code does not matter
  dwarf_dieoffset(entry, &offset, &error);

  source = unit->sources;

  while (source != NULL)
  {
    if (offset == source->offset)
    {
      dwarf_dealloc(unit->instance, entry, DW_DLA_DIE);
      return source;
    }

    source = source->next;
  }

  source = (struct SourceCache*)calloc(1, sizeof(struct SourceCache));

  source->entry  = entry;
  source->offset = offset;

  if ((dwarf_srclines_b(entry, &version, &source->count, &source->context, &error) == DW_DLV_OK) &&
      (dwarf_srclines_from_linecontext(source->context, &line, &source->length, &error) == DW_DLV_OK))
  {
    source->list = (struct SourceLine*)calloc(source->length, sizeof(struct SourceLine));
    limit        = line + source->length;
    last         = source->list;

    while (line < limit)
    {
      last->line  = line; 
      last       += (dwarf_lineaddr(*line, &last->address, &error) == DW_DLV_OK);
      line       ++;
    }

    // Update the length with count of resolved addresses
    source->length = last - source->list;

    qsort(source->list, source->length, sizeof(struct SourceLine), CompareAddresses);
  }

  source->next   = unit->sources;
  unit->sources  = source;

  return source;
}

int GetDebugInformation(Dl_info* information, struct link_map* map, uintptr_t address, struct DebugSourceInformation* buffer, int lock)
{
  Dwarf_Die entry;
  Dwarf_Error error;
  struct DebugUnit* unit;
  struct SourceLine* line;
  struct SourceLine* limit;
  struct SourceCache* source;

  if (information == NULL)
  {
    map         = NULL;
    information = (Dl_info*)alloca(sizeof(Dl_info));
    dladdr1((void*)address, information, (void**)&map, RTLD_DL_LINKMAP);
  }

  if ((map != NULL) &&
      (unit = GetDebugUnit(information->dli_fname, client)) &&
      (unit->instance != NULL) &&
      (address >= map->l_addr) &&
      ((lock == DEBUG_GET_LOCK_WAIT)      && (pthread_mutex_lock(&unit->lock)    == 0) ||
       (lock == DEBUG_GET_LOCK_DONT_WAIT) && (pthread_mutex_trylock(&unit->lock) == 0)))
  {
    address -= map->l_addr;

    buffer->instance = unit->instance;
    buffer->path     = NULL;
    buffer->line     = 0;
    buffer->column   = 0;
    buffer->address  = 0ULL;

    if ((entry  = GetDebugEntry(unit, address)) &&
        (source = GetSourceCache(unit, entry)))
    {
      line  = source->list;
      limit = source->list + source->length;

      while ((line < limit) &&
             (line->address < address))
      {
        // Simply walk line-by-line
        line ++;
      }

      if (line != limit)
      {
        buffer->address = line->address + map->l_addr;
        dwarf_linesrc(*line->line, &buffer->path, &error);
        dwarf_lineno(*line->line, &buffer->line, &error);
        dwarf_lineoff_b(*line->line, &buffer->column, &error);
        pthread_mutex_unlock(&unit->lock);
        return 1;
      }
    }

    pthread_mutex_unlock(&unit->lock);
  }

  return 0;
}

void ReleaseDebugInformation(struct DebugSourceInformation* information)
{
  if ((information->path     != NULL) &&
      (information->instance != NULL))
  {
    // https://chromium.googlesource.com/external/elfutils/+/elfutils-0.142/libdwarf/dwarf_dealloc.c
    dwarf_dealloc(information->instance, information->path, DW_DLA_STRING);
  }
}

// Unit preloading

static void AppendNameList(struct NameList* list, const char* name)
{
  int length;

  length = strlen(name) + 1;

  if ((list->length + length + 1) > list->size)
  {
    list->size += length + PATH_MAX;
    list->data  = (char*)realloc(list->data, list->size);
  }

  memcpy(list->data + list->length, name, length);

  list->length += length;
  list->data[list->length] = '\0';
}

static int HandleProgramHeader(struct dl_phdr_info* information, size_t size, void* data)
{
  struct stat status;

  if ((*information->dlpi_name != '\0') &&
      (stat(information->dlpi_name, &status) == 0))
  {
    // Retreive units with accessible path only
    AppendNameList((struct NameList*)data, information->dlpi_name);
  }

  return 0;
}

static void TryUpdateDebugCache(debuginfod_client* client)
{
  struct NameList list;
  char* name;

  list.data   = NULL;
  list.size   = 0;
  list.length = 0;

  name = (char*)alloca(PATH_MAX);
  readlink("/proc/self/exe", name, PATH_MAX);
  AppendNameList(&list, name);

  dl_iterate_phdr(HandleProgramHeader, &list);

  for (name = list.data; *name != '\0'; name += strlen(name) + 1)
  {
    // Retreive units slowly
    GetDebugUnit(name, client);
  }

  free(list.data);
}

static int HandleLoadProgress(debuginfod_client* client, long value1, long value2)
{
  return atomic_load_explicit(&state, memory_order_relaxed) != DEBUG_UPDATE_ASYNCHRONOUS;
}

static void* DoWork(void* argument)
{
  debuginfod_client* client;
  pthread_t thread;

  thread = pthread_self();
  client = debuginfod_begin();

  pthread_setname_np(thread, "Loader");
  debuginfod_set_progressfn(client, HandleLoadProgress);

  TryUpdateDebugCache(client);
  atomic_store_explicit(&state, DEBUG_UPDATE_SYNCHRONOUS, memory_order_relaxed);

  debuginfod_end(client);
  pthread_detach(thread);
  return NULL;
}

void UpdateDebugCache(int option)
{
  pthread_t thread;

  if (option == DEBUG_UPDATE_SYNCHRONOUS)
  {
    TryUpdateDebugCache(client);
    return;
  }

  if ((option == DEBUG_UPDATE_ASYNCHRONOUS) &&
      (atomic_exchange_explicit(&state, DEBUG_UPDATE_ASYNCHRONOUS, memory_order_relaxed) == DEBUG_UPDATE_SYNCHRONOUS) &&
      (pthread_create(&thread, NULL, DoWork, NULL) != 0))
  {
    atomic_store_explicit(&state, DEBUG_UPDATE_SYNCHRONOUS, memory_order_relaxed);
    return;
  }
}

void CancelUpdateDebugCache()
{
  atomic_store_explicit(&state, DEBUG_UPDATE_SYNCHRONOUS, memory_order_relaxed);
}
