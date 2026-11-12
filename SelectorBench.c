#include "Selector.h"

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

// Classic event loop object: an fd plus some state, allocated individually
struct Source
{
  int handle;
  uint32_t events;
  void (*function)(struct Source* source, uint32_t events);
  void* closure;
  char payload[40];
};

static volatile uint64_t sink;

__attribute__((noinline)) static void HandleSource(struct Source* source, uint32_t events)
{
  sink += (uint64_t)source->handle + events;
}

__attribute__((noinline)) static void HandleSelector(struct Selector* selector, int handle, uint32_t events, void* closure)
{
  sink += (uint64_t)handle + events;
}

static uint64_t Now(void)
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000000000ULL + (uint64_t)time.tv_nsec;
}

struct Case
{
  int count;                // registered sources
  int ready;                // ready sources
  int* handles;             // registered handle numbers
  struct Source** sources;  // classic list in registration order
  void** junk;              // spacers that scatter sources over the heap
  struct Selector* selector;
  size_t words;             // words covering the highest handle
  uint64_t* master;         // persistent registration mask for the classic variant
  uint64_t* image;          // simulated select() result
  fd_set* readable;         // classic working set
  int limit;                // classic nfds
};

static void Shuffle(int* array, int count)
{
  for (int index = count - 1; index > 0; index --)
  {
    int other = rand() % (index + 1);
    int value = array[index];
    array[index] = array[other];
    array[other] = value;
  }
}

static void BuildCase(struct Case* test, int count, int ready, const int* handles)
{
  int* order;

  memset(test, 0, sizeof(struct Case));
  test->count    = count;
  test->ready    = ready;
  test->handles  = (int*)malloc(sizeof(int) * (size_t)count);
  test->sources  = (struct Source**)malloc(sizeof(struct Source*) * (size_t)count);
  test->junk     = (void**)malloc(sizeof(void*) * (size_t)count);
  test->selector = CreateSelector();
  memcpy(test->handles, handles, sizeof(int) * (size_t)count);

  for (int index = 0; index < count; index ++)
  {
    if (test->handles[index] + 1 > test->limit)
      test->limit = test->handles[index] + 1;
  }

  test->words    = ((size_t)test->limit + 63) / 64;
  test->master   = (uint64_t*)calloc(test->words, sizeof(uint64_t));
  test->image    = (uint64_t*)calloc(test->words, sizeof(uint64_t));
  test->readable = (fd_set*)calloc(test->words, sizeof(uint64_t));

  for (int index = 0; index < count; index ++)
  {
    struct Source* source = (struct Source*)malloc(sizeof(struct Source));
    test->junk[index]     = malloc(64 + (size_t)(rand() % 512));
    source->handle        = test->handles[index];
    source->events        = SELECTOR_EVENT_READ;
    source->function      = HandleSource;
    source->closure       = NULL;
    test->sources[index]  = source;
    test->master[source->handle / 64] |= 1ULL << (source->handle % 64);
    SetSelectorHandler(test->selector, source->handle, SELECTOR_EVENT_READ, HandleSelector, source);
  }

  order = (int*)malloc(sizeof(int) * (size_t)count);

  for (int index = 0; index < count; index ++)
    order[index] = index;

  Shuffle(order, count);

  for (int index = 0; index < ready; index ++)
  {
    int handle = test->handles[order[index]];
    test->image[handle / 64] |= 1ULL << (handle % 64);
  }

  free(order);
}

static void FreeCase(struct Case* test)
{
  for (int index = 0; index < test->count; index ++)
  {
    free(test->sources[index]);
    free(test->junk[index]);
  }

  ReleaseSelector(test->selector);
  free(test->sources);
  free(test->junk);
  free(test->handles);
  free(test->master);
  free(test->image);
  free(test->readable);
}

// Simulated loops: select() is replaced by copying the prepared result image

static void ClassicRebuild(void* argument)
{
  struct Case* test = (struct Case*)argument;

  memset(test->readable, 0, test->words * sizeof(uint64_t));

  for (int index = 0; index < test->count; index ++)
    FD_SET(test->sources[index]->handle, test->readable);

  memcpy(test->readable, test->image, test->words * sizeof(uint64_t));

  for (int index = 0; index < test->count; index ++)
  {
    struct Source* source = test->sources[index];

    if (FD_ISSET(source->handle, test->readable))
      source->function(source, SELECTOR_EVENT_READ);
  }
}

static void ClassicPersistent(void* argument)
{
  struct Case* test = (struct Case*)argument;

  memcpy(test->readable, test->master, test->words * sizeof(uint64_t));
  memcpy(test->readable, test->image,  test->words * sizeof(uint64_t));

  for (int index = 0; index < test->count; index ++)
  {
    struct Source* source = test->sources[index];

    if (FD_ISSET(source->handle, test->readable))
      source->function(source, SELECTOR_EVENT_READ);
  }
}

static void SelectorLoop(void* argument)
{
  struct Case* test = (struct Case*)argument;
  fd_set* readable;
  fd_set* writable;
  fd_set* exceptional;

  PrepareSelector(test->selector, &readable, &writable, &exceptional);
  memcpy(readable, test->image, test->words * sizeof(uint64_t));
  DispatchSelector(test->selector, test->ready);
}

static double Measure(void (*function)(void*), void* test)
{
  uint64_t start;
  uint64_t elapsed;
  uint64_t iterations;
  double best;

  // Calibrate to about 20 ms per sample, take the best of 7 samples
  for (iterations = 16; ; iterations *= 2)
  {
    start = Now();
    for (uint64_t index = 0; index < iterations; index ++)
      function(test);
    if ((elapsed = Now() - start) > 20000000ULL)
      break;
  }

  best = (double)elapsed / (double)iterations;

  for (int sample = 0; sample < 7; sample ++)
  {
    start = Now();
    for (uint64_t index = 0; index < iterations; index ++)
      function(test);
    elapsed = Now() - start;
    if ((double)elapsed / (double)iterations < best)
      best = (double)elapsed / (double)iterations;
  }

  return best;
}

// Real select() over pipes

struct Real
{
  int count;
  int ready;
  int (*pipes)[2];
  struct Source** sources;
  struct Selector* selector;
  fd_set* readable;
  size_t words;
  int limit;
};

static void BuildReal(struct Real* test, int count, int ready)
{
  int* order;

  memset(test, 0, sizeof(struct Real));
  test->count    = count;
  test->ready    = ready;
  test->pipes    = malloc(sizeof(int[2]) * (size_t)count);
  test->sources  = (struct Source**)malloc(sizeof(struct Source*) * (size_t)count);
  test->selector = CreateSelector();

  for (int index = 0; index < count; index ++)
  {
    struct Source* source;

    if (pipe(test->pipes[index]) != 0)
    {
      perror("pipe");
      exit(1);
    }

    source               = (struct Source*)calloc(1, sizeof(struct Source));
    source->handle       = test->pipes[index][0];
    source->events       = SELECTOR_EVENT_READ;
    source->function     = HandleSource;
    test->sources[index] = source;

    if (source->handle + 1 > test->limit)
      test->limit = source->handle + 1;

    SetSelectorHandler(test->selector, source->handle, SELECTOR_EVENT_READ, HandleSelector, source);
  }

  test->words    = ((size_t)test->limit + 63) / 64;
  test->readable = (fd_set*)calloc(test->words, sizeof(uint64_t));

  order = (int*)malloc(sizeof(int) * (size_t)count);

  for (int index = 0; index < count; index ++)
    order[index] = index;

  Shuffle(order, count);

  // Data is never read, so these stay readable on every iteration
  for (int index = 0; index < ready; index ++)
  {
    if (write(test->pipes[order[index]][1], "x", 1) != 1)
    {
      perror("write");
      exit(1);
    }
  }

  free(order);
}

static void FreeReal(struct Real* test)
{
  for (int index = 0; index < test->count; index ++)
  {
    close(test->pipes[index][0]);
    close(test->pipes[index][1]);
    free(test->sources[index]);
  }

  ReleaseSelector(test->selector);
  free(test->pipes);
  free(test->sources);
  free(test->readable);
}

static void RealClassic(void* argument)
{
  struct Real* test = (struct Real*)argument;
  struct timeval timeout = { 0, 0 };
  int result;

  memset(test->readable, 0, test->words * sizeof(uint64_t));

  for (int index = 0; index < test->count; index ++)
    FD_SET(test->sources[index]->handle, test->readable);

  result = select(test->limit, test->readable, NULL, NULL, &timeout);

  if (result != test->ready)
    abort();

  for (int index = 0; index < test->count; index ++)
  {
    struct Source* source = test->sources[index];

    if (FD_ISSET(source->handle, test->readable))
      source->function(source, SELECTOR_EVENT_READ);
  }
}

static void RealSelector(void* argument)
{
  struct Real* test = (struct Real*)argument;
  struct timeval timeout = { 0, 0 };
  fd_set* readable;
  fd_set* writable;
  fd_set* exceptional;
  int count;

  count = PrepareSelector(test->selector, &readable, &writable, &exceptional);
  count = select(count, readable, writable, exceptional, &timeout);

  if (count != test->ready)
    abort();

  DispatchSelector(test->selector, count);
}

static void RealSelectorRead(void* argument)
{
  struct Real* test = (struct Real*)argument;
  struct timeval timeout = { 0, 0 };
  fd_set* readable;
  fd_set* writable;
  fd_set* exceptional;
  int count;

  // Same as RealSelector, but the empty write / except sets are not passed to the kernel
  count = PrepareSelector(test->selector, &readable, &writable, &exceptional);
  count = select(count, readable, NULL, NULL, &timeout);

  if (count != test->ready)
    abort();

  DispatchSelector(test->selector, count);
}

static void RunSimulated(void)
{
  static const int counts[] = { 16, 64, 256, 1024, 4096 };
  struct Case test;
  int* handles;

  printf("Simulated select(): user-space cost only, ns per iteration\n\n");
  printf("%6s %6s | %10s %10s %10s | %8s\n", "N", "R", "rebuild", "persistent", "Selector", "speedup");

  for (size_t item = 0; item < sizeof(counts) / sizeof(counts[0]); item ++)
  {
    int count = counts[item];
    int readies[] = { 1, 4, count / 10, count };

    // Dense handle numbers starting after stdio, as a real process would get them
    handles = (int*)malloc(sizeof(int) * (size_t)count);

    for (int index = 0; index < count; index ++)
      handles[index] = 3 + index;

    for (size_t choice = 0; choice < 4; choice ++)
    {
      double rebuild, persistent, selector;

      if ((choice > 0) && (readies[choice] <= readies[choice - 1]))
        continue;

      BuildCase(&test, count, readies[choice], handles);
      rebuild    = Measure(ClassicRebuild,    &test);
      persistent = Measure(ClassicPersistent, &test);
      selector   = Measure(SelectorLoop,      &test);
      printf("%6d %6d | %10.1f %10.1f %10.1f | %7.1fx\n", count, readies[choice], rebuild, persistent, selector, rebuild / selector);
      FreeCase(&test);
    }

    free(handles);
  }
}

static void RunReal(void)
{
  static const int counts[] = { 16, 64, 256, 1024, 4096 };
  struct Real real;

  printf("Real select() over pipes, zero timeout, ns per iteration\n\n");
  printf("%6s %6s | %10s %10s %10s | %8s %8s\n", "N", "R", "classic", "Selector", "read-only", "speedup", "read-only");

  for (size_t item = 0; item < sizeof(counts) / sizeof(counts[0]); item ++)
  {
    int count = counts[item];
    int readies[] = { 1, count / 10, count };

    for (size_t choice = 0; choice < 3; choice ++)
    {
      double classic, selector, only;

      if ((choice > 0) && (readies[choice] <= readies[choice - 1]))
        continue;

      BuildReal(&real, count, readies[choice]);
      classic  = Measure(RealClassic,      &real);
      selector = Measure(RealSelector,     &real);
      only     = Measure(RealSelectorRead, &real);
      printf("%6d %6d | %10.1f %10.1f %10.1f | %7.2fx %7.2fx\n", count, readies[choice], classic, selector, only, classic / selector, classic / only);
      FreeReal(&real);
    }
  }
}

// Usage: SelectorBench [simulated | real], both when omitted
int main(int count, char** arguments)
{
  struct rlimit limit;
  const char* mode;

  srand(1);

  // Real benchmark needs 2 * 4096 descriptors and more
  getrlimit(RLIMIT_NOFILE, &limit);
  limit.rlim_cur = (limit.rlim_max < 20000) ? limit.rlim_max : 20000;
  setrlimit(RLIMIT_NOFILE, &limit);

  mode = (count > 1) ? arguments[1] : NULL;

  if ((mode != NULL) && (strcmp(mode, "simulated") != 0) && (strcmp(mode, "real") != 0))
  {
    fprintf(stderr, "Usage: %s [simulated | real]\n", arguments[0]);
    return 1;
  }

  if ((mode == NULL) || (strcmp(mode, "simulated") == 0))
    RunSimulated();

  if (mode == NULL)
    printf("\n");

  if ((mode == NULL) || (strcmp(mode, "real") == 0))
    RunReal();

  return (int)(sink & 0);
}
