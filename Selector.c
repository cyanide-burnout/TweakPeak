#include "Selector.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "Selector relies on the little-endian layout of fd_set"
#endif

#define KIND_COUNT  3
#define WORD_BITS   64
#define EVENT_ALL   (SELECTOR_EVENT_READ | SELECTOR_EVENT_WRITE | SELECTOR_EVENT_EXCEPT)

#define STATE_IDLE      0
#define STATE_PREPARE   1
#define STATE_DISPATCH  2

struct SelectorEntry
{
  SelectorHandlerFunction function;
  void* closure;
};

struct Selector
{
  int state;                     // STATE_PREPARE between Prepare and Dispatch, STATE_DISPATCH while handlers are being called
  size_t length;                 // number of words in each set, size = length * WORD_BITS
  uint64_t* masks[KIND_COUNT];   // registrations, persistent
  uint64_t* events[KIND_COUNT];  // results, all zero outside of a dispatch
  struct SelectorEntry* entries;
};

static int ResizeArray(void** array, size_t previous, size_t size)
{
  void* pointer;

  pointer = realloc(*array, size);

  if (pointer == NULL)
  {
    errno = ENOMEM;
    return -1;
  }

  memset((uint8_t*)pointer + previous, 0, size - previous);
  *array = pointer;
  return 0;
}

static int GrowSelector(struct Selector* selector, size_t length)
{
  size_t previous;
  size_t size;

  // An array that has already grown before a failure stays valid with the old length
  // and is simply resized again by the next attempt.
  // Safe during a dispatch: the loop reloads these pointers on every access
  previous = selector->length * sizeof(uint64_t);
  size     = length * sizeof(uint64_t);

  if ((ResizeArray((void**)&selector->masks[0],  previous, size) < 0) ||
      (ResizeArray((void**)&selector->masks[1],  previous, size) < 0) ||
      (ResizeArray((void**)&selector->masks[2],  previous, size) < 0) ||
      (ResizeArray((void**)&selector->events[0], previous, size) < 0) ||
      (ResizeArray((void**)&selector->events[1], previous, size) < 0) ||
      (ResizeArray((void**)&selector->events[2], previous, size) < 0) ||
      (ResizeArray((void**)&selector->entries,   selector->length * WORD_BITS * sizeof(struct SelectorEntry), length * WORD_BITS * sizeof(struct SelectorEntry)) < 0))
    return -1;

  selector->length = length;
  return 0;
}

struct Selector* CreateSelector(void)
{
  struct Selector* selector;

  if ((selector = (struct Selector*)calloc(1, sizeof(struct Selector))) == NULL)
  {
    errno = ENOMEM;
    return NULL;
  }

  if (GrowSelector(selector, 1) < 0)
  {
    ReleaseSelector(selector);
    errno = ENOMEM;
    return NULL;
  }

  return selector;
}

void ReleaseSelector(struct Selector* selector)
{
  if (selector != NULL)
  {
    free(selector->masks[0]);
    free(selector->masks[1]);
    free(selector->masks[2]);
    free(selector->events[0]);
    free(selector->events[1]);
    free(selector->events[2]);
    free(selector->entries);
    free(selector);
  }
}

int SetSelectorHandler(struct Selector* selector, int handle, uint32_t events, SelectorHandlerFunction function, void* closure)
{
  struct SelectorEntry* entry;
  uint64_t mask;
  size_t length;
  size_t index;

  if ((handle < 0) ||
      (events & ~EVENT_ALL) ||
      (events && (function == NULL)))
  {
    errno = EINVAL;
    return -1;
  }

  index = (size_t)handle / WORD_BITS;
  mask  = 1ULL << (handle % WORD_BITS);

  if (index >= selector->length)
  {
    // Removal of a handle that has never fit cannot have anything to drop
    if (!events)
      return 0;

    length = selector->length * 2;

    if (length <= index)
      length = index + 1;

    if (GrowSelector(selector, length) < 0)
      return -1;
  }

  for (int kind = 0; kind < KIND_COUNT; kind ++)
  {
    if (events & (1U << kind))
    {
      selector->masks[kind][index] |= mask;
      continue;
    }

    // Dropping the pending result keeps events a subset of masks,
    // so the dispatch loop never sees an unregistered event
    selector->masks[kind][index]  &= ~mask;
    selector->events[kind][index] &= ~mask;
  }

  entry           = selector->entries + handle;
  entry->function = events ? function : NULL;
  entry->closure  = events ? closure  : NULL;

  return 0;
}

int PrepareSelector(struct Selector* selector, fd_set** readable, fd_set** writable, fd_set** exceptional)
{
  if (selector->state == STATE_DISPATCH)
  {
    errno = EDEADLK;
    return -1;
  }

  memcpy(selector->events[0], selector->masks[0], selector->length * sizeof(uint64_t));
  memcpy(selector->events[1], selector->masks[1], selector->length * sizeof(uint64_t));
  memcpy(selector->events[2], selector->masks[2], selector->length * sizeof(uint64_t));

  *readable    = (fd_set*)selector->events[0];
  *writable    = (fd_set*)selector->events[1];
  *exceptional = (fd_set*)selector->events[2];

  selector->state = STATE_PREPARE;

  // The whole storage goes to select(): an empty tail costs a few zero words,
  // the kernel limits nfds to its own descriptor table anyway
  return (int)(selector->length * WORD_BITS);
}

int DispatchSelector(struct Selector* selector, int result)
{
  struct SelectorEntry* entry;
  uint64_t value;
  uint32_t events;
  size_t length;
  int number;
  int handle;
  int count;

  if (selector->state != STATE_PREPARE)
  {
    errno = (selector->state == STATE_DISPATCH) ? EDEADLK : EINVAL;
    return -1;
  }

  // Storage may have grown since PrepareSelector(), the new words hold no results
  length = selector->length;

  if (result <= 0)
  {
    // Contents of the sets are unspecified on error, keep the invariant.
    // errno of select() is left untouched
    memset(selector->events[0], 0, length * sizeof(uint64_t));
    memset(selector->events[1], 0, length * sizeof(uint64_t));
    memset(selector->events[2], 0, length * sizeof(uint64_t));

    selector->state = STATE_IDLE;
    return result;
  }

  count           = 0;
  selector->state = STATE_DISPATCH;

  // Handles registered during the dispatch beyond length have no results,
  // handles removed during the dispatch have their results already dropped.
  // A handler may grow the storage, so set and entry pointers are reloaded
  // from the selector after every call
  for (size_t index = 0; index < length; index ++)
  {
    while (value = selector->events[0][index] | selector->events[1][index] | selector->events[2][index])
    {
      number = __builtin_ctzll(value);
      handle = (int)(index * WORD_BITS + (size_t)number);
      events = (uint32_t)(
        (((selector->events[0][index] >> number) & 1) << 0) |
        (((selector->events[1][index] >> number) & 1) << 1) |
        (((selector->events[2][index] >> number) & 1) << 2));

      // Bits below number are zero in every set, so value - 1 clears exactly bit number
      selector->events[0][index] &= value - 1;
      selector->events[1][index] &= value - 1;
      selector->events[2][index] &= value - 1;

      entry = selector->entries + handle;
      entry->function(selector, handle, events, entry->closure);
      count ++;
    }
  }

  selector->state = STATE_IDLE;
  return count;
}
