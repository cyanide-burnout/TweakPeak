#include "Selector.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>

#define CHECK(condition)  do { if (!(condition)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

struct Record
{
  int calls;
  int handle;
  uint32_t events;
};

static void Record(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  struct Record* record = (struct Record*)closure;
  record->calls ++;
  record->handle = handle;
  record->events = events;
}

// Prepare -> select -> Dispatch, the loop body a caller writes

static int WaitSelector(struct Selector* selector, struct timeval* timeout)
{
  fd_set* readable;
  fd_set* writable;
  fd_set* exceptional;
  int count;

  if ((count = PrepareSelector(selector, &readable, &writable, &exceptional)) < 0)
    return count;

  return DispatchSelector(selector, select(count, readable, writable, exceptional, timeout));
}

static struct timeval Zero(void)
{
  struct timeval value = { 0, 0 };
  return value;
}

// Test 2: close and reuse a pending handle from another handler

struct Reuse
{
  int victim;
  int reused;
  int writer;
  struct Record* old;
  struct Record* fresh;
};

static void CloseAndReuse(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  struct Reuse* state = (struct Reuse*)closure;
  int pair[2];

  CHECK(SetSelectorHandler(selector, state->victim, 0, NULL, NULL) == 0);
  close(state->victim);
  CHECK(pipe(pair) == 0);
  state->reused = pair[0];
  state->writer = pair[1];
  CHECK(SetSelectorHandler(selector, pair[0], SELECTOR_EVENT_READ, Record, state->fresh) == 0);
}

// Test 4: drop one event of another pending handle

struct Drop
{
  int target;
  struct Record* record;
};

static void DropRead(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  struct Drop* state = (struct Drop*)closure;
  CHECK(SetSelectorHandler(selector, state->target, SELECTOR_EVENT_WRITE, Record, state->record) == 0);
}

// Test 5: nested wait

static int nested = 0;

static void Nest(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  struct timeval timeout = Zero();
  nested = WaitSelector(selector, &timeout) == -1 && errno == EDEADLK;
}

// Test 7: grow the storage from a handler

struct Grow
{
  int source;
  int done;
  struct Record* record;
};

static void GrowFromHandler(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  struct Grow* state = (struct Grow*)closure;

  if (!state->done)
  {
    CHECK(dup2(state->source, 3000) == 3000);
    CHECK(SetSelectorHandler(selector, 3000, SELECTOR_EVENT_READ, Record, state->record) == 0);
    state->done = 1;
  }
}

int main(void)
{
  struct rlimit limit = { 4096, 4096 };
  struct timeval timeout;
  struct Selector* selector;
  int a[2], b[2], s[2];

  getrlimit(RLIMIT_NOFILE, &limit);
  if (limit.rlim_cur < 4096)
  {
    limit.rlim_cur = limit.rlim_max < 4096 ? limit.rlim_max : 4096;
    setrlimit(RLIMIT_NOFILE, &limit);
  }

  CHECK(selector = CreateSelector());

  // 1. Basic read
  {
    struct Record record = { 0 };
    CHECK(pipe(a) == 0);
    CHECK(SetSelectorHandler(selector, a[0], SELECTOR_EVENT_READ, Record, &record) == 0);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 0);
    CHECK(write(a[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 1);
    CHECK(record.calls == 1 && record.handle == a[0] && record.events == SELECTOR_EVENT_READ);
    CHECK(SetSelectorHandler(selector, a[0], 0, NULL, NULL) == 0);
    close(a[0]); close(a[1]);
    printf("ok 1 basic\n");
  }

  // 2. Close + reuse of a pending handle inside the same pass
  {
    struct Record old = { 0 }, fresh = { 0 };
    struct Reuse state = { 0, -1, -1, &old, &fresh };
    CHECK(pipe(a) == 0);
    CHECK(pipe(b) == 0);
    CHECK(a[0] < b[0]);
    state.victim = b[0];
    CHECK(SetSelectorHandler(selector, a[0], SELECTOR_EVENT_READ, CloseAndReuse, &state) == 0);
    CHECK(SetSelectorHandler(selector, b[0], SELECTOR_EVENT_READ, Record, &old) == 0);
    CHECK(write(a[1], "x", 1) == 1);
    CHECK(write(b[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 1);
    CHECK(state.reused == b[0]);  // the number really was reused
    CHECK(old.calls == 0);
    CHECK(fresh.calls == 0);
    CHECK(SetSelectorHandler(selector, a[0], 0, NULL, NULL) == 0);
    CHECK(SetSelectorHandler(selector, state.reused, 0, NULL, NULL) == 0);
    close(a[0]); close(a[1]); close(b[1]); close(state.reused); close(state.writer);
    printf("ok 2 reuse (handle %d)\n", state.reused);
  }

  // 3. Handles across words and above FD_SETSIZE
  {
    struct Record low = { 0 }, middle = { 0 }, high = { 0 };
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(dup2(s[0], 130) == 130);
    CHECK(dup2(s[0], 1500) == 1500);
    CHECK(SetSelectorHandler(selector, s[0], SELECTOR_EVENT_READ, Record, &low) == 0);
    CHECK(SetSelectorHandler(selector, 130,  SELECTOR_EVENT_READ, Record, &middle) == 0);
    CHECK(SetSelectorHandler(selector, 1500, SELECTOR_EVENT_READ, Record, &high) == 0);
    CHECK(write(s[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 3);
    CHECK(low.calls == 1 && middle.calls == 1 && high.calls == 1 && high.handle == 1500);
    CHECK(SetSelectorHandler(selector, 1500, 0, NULL, NULL) == 0);
    CHECK(SetSelectorHandler(selector, 130, 0, NULL, NULL) == 0);
    CHECK(SetSelectorHandler(selector, s[0], 0, NULL, NULL) == 0);
    close(1500); close(130); close(s[0]); close(s[1]);
    printf("ok 3 large handles\n");
  }

  // 4. Dropping one event of a pending handle keeps the other
  {
    struct Record record = { 0 };
    struct Drop state;
    CHECK(pipe(a) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(a[0] < s[0]);
    state.target = s[0];
    state.record = &record;
    CHECK(SetSelectorHandler(selector, a[0], SELECTOR_EVENT_READ, DropRead, &state) == 0);
    CHECK(SetSelectorHandler(selector, s[0], SELECTOR_EVENT_READ | SELECTOR_EVENT_WRITE, Record, &record) == 0);
    CHECK(write(a[1], "x", 1) == 1);
    CHECK(write(s[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 2);
    CHECK(record.calls == 1 && record.events == SELECTOR_EVENT_WRITE);
    CHECK(SetSelectorHandler(selector, a[0], 0, NULL, NULL) == 0);
    CHECK(SetSelectorHandler(selector, s[0], 0, NULL, NULL) == 0);
    close(a[0]); close(a[1]); close(s[0]); close(s[1]);
    printf("ok 4 partial drop\n");
  }

  // 5. Nested wait is rejected
  {
    CHECK(pipe(a) == 0);
    CHECK(SetSelectorHandler(selector, a[0], SELECTOR_EVENT_READ, Nest, NULL) == 0);
    CHECK(write(a[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 1);
    CHECK(nested);
    CHECK(SetSelectorHandler(selector, a[0], 0, NULL, NULL) == 0);
    close(a[0]); close(a[1]);
    printf("ok 5 nested\n");
  }

  // 6. Invalid arguments
  {
    CHECK(SetSelectorHandler(selector, -1, SELECTOR_EVENT_READ, Record, NULL) == -1 && errno == EINVAL);
    CHECK(SetSelectorHandler(selector, 100000, 0, NULL, NULL) == 0);  // removal beyond size is a no-op
    CHECK(SetSelectorHandler(selector, 3, 1 << 5, Record, NULL) == -1 && errno == EINVAL);
    CHECK(SetSelectorHandler(selector, 3, SELECTOR_EVENT_READ, NULL, NULL) == -1 && errno == EINVAL);
    CHECK(DispatchSelector(selector, 1) == -1 && errno == EINVAL);  // no PrepareSelector()
    errno = EINTR;
    {
      fd_set* r; fd_set* w; fd_set* e;
      CHECK(PrepareSelector(selector, &r, &w, &e) > 0);
      CHECK(DispatchSelector(selector, -1) == -1 && errno == EINTR);  // select() error passes through
      CHECK(DispatchSelector(selector, 1) == -1 && errno == EINVAL);  // back to idle
    }
    printf("ok 6 arguments\n");
  }

  ReleaseSelector(selector);

  // 7. Storage grows from a handler while later results are still pending
  {
    struct Record pending = { 0 }, grown = { 0 };
    struct Grow state;
    CHECK(selector = CreateSelector());
    CHECK(pipe(a) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, s) == 0);
    CHECK(dup2(s[0], 100) == 100);
    state.source = s[0];
    state.done   = 0;
    state.record = &grown;
    CHECK(SetSelectorHandler(selector, a[0], SELECTOR_EVENT_READ, GrowFromHandler, &state) == 0);
    CHECK(SetSelectorHandler(selector, 100, SELECTOR_EVENT_READ, Record, &pending) == 0);
    CHECK(write(a[1], "x", 1) == 1);
    CHECK(write(s[1], "x", 1) == 1);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 2);
    CHECK(pending.calls == 1 && pending.handle == 100);
    CHECK(grown.calls == 0);
    timeout = Zero();
    CHECK(WaitSelector(selector, &timeout) == 3);  // a[0], 100 and 3000 are all still readable
    CHECK(grown.calls == 1 && grown.handle == 3000);
    ReleaseSelector(selector);
    close(3000); close(100); close(a[0]); close(a[1]); close(s[0]); close(s[1]);
    printf("ok 7 growth during dispatch\n");
  }

  return 0;
}
