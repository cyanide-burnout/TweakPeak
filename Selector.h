#ifndef SELECTOR_H
#define SELECTOR_H

// Must be seen before the first inclusion of <sys/select.h> in the translation
// unit that calls select(): include Selector.h first or pass it with -D
#ifndef _DARWIN_UNLIMITED_SELECT
#define _DARWIN_UNLIMITED_SELECT
#endif

#include <stdint.h>
#include <stddef.h>
#include <sys/select.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
  Selector: select()-based dispatcher that walks the ready bitmaps
  returned by the kernel instead of the list of registered handlers.

  The wait itself stays with the caller: PrepareSelector() fills the
  sets and returns nfds, the caller passes them to select() / pselect()
  together with its own timeout and signal mask, DispatchSelector()
  takes the result and calls the handlers:

    fd_set* readable;
    fd_set* writable;
    fd_set* exceptional;
    int count;

    count = PrepareSelector(selector, &readable, &writable, &exceptional);
    count = select(count, readable, writable, exceptional, timeout);
    count = DispatchSelector(selector, count);

  User-space cost of one pass is O(size / 64 + R),
  where size is the current storage size in handles and R is the
  number of ready handles. Handler lookup is a direct array index.

  Storage grows on demand when a handle beyond its current size is
  registered (doubling, may exceed FD_SETSIZE) and never shrinks:
  a handle number reached once will be reached again. The whole
  storage is passed to select() as nfds. Growth is allowed from
  handlers during a dispatch.

  This relies on:

  1. fd_set being a plain bit array where handle N is bit N % W of
     word N / W. That holds for glibc (W = 64, long) and Darwin
     (W = 32, int32_t). On a little-endian machine both layouts are
     bit-identical to an array of uint64_t, which is what Selector
     keeps and passes to select() (checked at build time).

  2. select() accepting sets larger than FD_SETSIZE. Linux does that
     natively. Darwin returns EINVAL for nfds > FD_SETSIZE unless
     _DARWIN_UNLIMITED_SELECT is defined, which links
     select$DARWIN_EXTSN instead. The caller's translation unit is
     the one that links select(), so the macro has to be in effect
     there: Selector.h defines it, which works only if nothing has
     included <sys/select.h> before it. Otherwise define it project
     wide (-D_DARWIN_UNLIMITED_SELECT). The practical ceiling is then
     RLIMIT_NOFILE, bounded by kern.maxfilesperproc.

  Reentrancy:

  - Handlers may call SetSelectorHandler() for any handle, including
    their own, while DispatchSelector() is calling handlers. Removing an event
    from a handle also drops that event from the pending results of
    the current pass, so a handler is never called for an event it
    is no longer registered for.

  - A handle must be removed (events = 0) before it is closed. Removal
    drops all its pending results, so when the number is reused by
    a new owner within the same pass (open() / socket() return the
    lowest free number), the new owner never receives the events
    of the old one.

  - The sets returned by PrepareSelector() stay valid until the next
    SetSelectorHandler() that grows the storage, so nothing should be
    registered between PrepareSelector() and the return of select().
    Changes between select() and DispatchSelector() are safe.

  - Handlers must not call PrepareSelector() / DispatchSelector()
    (fail with EDEADLK) or ReleaseSelector(), and must not unwind out
    of the dispatch (C++ exceptions, longjmp).
*/

#define SELECTOR_EVENT_READ    (1 << 0)
#define SELECTOR_EVENT_WRITE   (1 << 1)
#define SELECTOR_EVENT_EXCEPT  (1 << 2)

struct Selector;

typedef void (*SelectorHandlerFunction)(struct Selector* selector, int handle, uint32_t events, void* closure);

struct Selector* CreateSelector(void);
void ReleaseSelector(struct Selector* selector);

// events = 0 removes the handle, returns 0 or -1 with errno set (EINVAL, ENOMEM)
int SetSelectorHandler(struct Selector* selector, int handle, uint32_t events, SelectorHandlerFunction function, void* closure);

// Copies registrations into the result sets, returns nfds for select() or -1 with errno set
int PrepareSelector(struct Selector* selector, fd_set** readable, fd_set** writable, fd_set** exceptional);

// result: the return value of select(). Returns the number of handlers called,
// or result itself when it is 0 or -1 (errno of select() is preserved),
// or -1 with EINVAL when PrepareSelector() was not called
int DispatchSelector(struct Selector* selector, int result);

#ifdef __cplusplus
};
#endif

#endif
