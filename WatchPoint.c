#define _GNU_SOURCE
#define __USE_POSIX199309

#include "WatchPoint.h"

// https://stackoverflow.com/questions/8941711/is-is-possible-to-set-a-gdb-watchpoint-programatically
// https://stackoverflow.com/questions/40818920/how-to-set-the-value-of-dr7-register-in-order-to-create-a-hardware-breakpoint-on
// http://x86asm.net/articles/debugging-in-amd64-64-bit-mode-in-theory/
// http://www.cs.usfca.edu/~cruse/cs635s05/hdtraps.c
// https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3b-part-2-manual.pdf 17.2

// https://aarzilli.github.io/debugger-bibliography/hwbreak.html
// https://stackoverflow.com/questions/68755446/setting-a-hardware-breakpoint-in-arm64
// https://android.googlesource.com/platform/bionic/+/master/tests/sys_ptrace_test.cpp  <--
// http://stanshebs.github.io/gdb-doxy-test/gdbserver/linux-aarch64-low_8c_source.html
// https://developer.arm.com/documentation/dui0446/z/controlling-target-execution/overview--breakpoints-and-watchpoints

// https://topic.alibabacloud.com/a/how-to-set-a-hardware-breakpoint-in-your-program-set-data-breakpoints-with-program-code-instead-of-jtag_8_8_10268135.html
// https://developer.arm.com/documentation/ddi0406/cb/Debug-Architecture/The-Debug-Registers/Register-descriptions--in-register-order/DBGBCR--Breakpoint-Control-Registers
// https://github.com/trixirt/deebe/blob/master/src/linux-arm.c

#include <stdlib.h>

#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <syslog.h>
#include <pthread.h>
#include <syscall.h>

#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>

#include <linux/elf.h>
#include <linux/ptrace.h>

#include <string.h>
#include <semaphore.h>
#include <stdatomic.h>

#define SIGNAL      (SIGRTMAX - 1)
#define STACK_SIZE  (512 * 1024)

#define STATE_STOP  0
#define STATE_RUN   1

#if defined(__i386__) || defined(__x86_64__)
#define DR(number)          offsetof(struct user, u_debugreg[number])
#define DR7_BREAK_MASK      (0b11   <<  0)
#define DR7_CONDITION_MASK  (0b1111 << 16)

struct DebugRegisterState
{
  const void* addresses[4];
  uint32_t control;
};
#endif

#if defined(__arm__)
#define HWP  0  // Watch
#define HBP  1  // Break
#define WCR_TYPE_MASK  WATCHPOINT_BREAK_ON_READWRITE

struct DebugRegisterState
{
  struct
  {
    const void* address;
    uint32_t control[2];
  } state[16];
};
#endif

#if defined(__aarch64__)
#define HWP  0  // Watch
#define HBP  1  // Break
#define WCR_TYPE_MASK  WATCHPOINT_BREAK_ON_READWRITE

struct DebugRegisterState
{
  struct user_hwdebug_state state[2];
};
#endif

struct WatchContext
{
  struct DebugRegisterState set;
  atomic_int state;
  sem_t semaphore;
  pid_t process;
  void* stack;
  int status;
  int error;
};

// Platform-specific routines

#if defined(__i386__) || defined(__x86_64__)
static void SetDebugRegister(struct WatchContext* context, int number, const void* address, uint32_t condition)
{
  number &= 3;
  context->set.addresses[number] = address;
  context->set.control &= ~(DR7_BREAK_MASK                 << (number * 2));
  context->set.control &= ~(DR7_CONDITION_MASK             << (number * 4));
  context->set.control |= (address != NULL)                << (number * 2);
  context->set.control |= (condition & DR7_CONDITION_MASK) << (number * 4);
}

static void SaveDebugRegisterState(struct WatchContext* context, siginfo_t* information)
{
  uint32_t status;

  status  = ptrace(PTRACE_PEEKUSER, context->process, DR(6), 0);
  status &= 0b1111;

  context->status = 
    (status >= 0b0001) +
    (status >= 0b0010) +
    (status >= 0b0100) +
    (status >= 0b1000) -
    1;
}

static void LoadDebugRegisterState(struct WatchContext* context)
{
  ptrace(PTRACE_POKEUSER, context->process, DR(0), context->set.addresses[0]);
  ptrace(PTRACE_POKEUSER, context->process, DR(1), context->set.addresses[1]);
  ptrace(PTRACE_POKEUSER, context->process, DR(2), context->set.addresses[2]);
  ptrace(PTRACE_POKEUSER, context->process, DR(3), context->set.addresses[3]);
  ptrace(PTRACE_POKEUSER, context->process, DR(7), context->set.control);
  ptrace(PTRACE_POKEUSER, context->process, DR(6), 0);
}
#endif

#if defined(__arm__)
static void SetDebugRegister(struct WatchContext* context, int number, const void* address, uint32_t condition)
{
  number &= 15;
  context->set.state[number].address      = address;
  context->set.state[number].control[HWP] = condition | ((condition & WCR_TYPE_MASK) != WATCHPOINT_BREAK_ON_EXECUTE) && (address != NULL);
  context->set.state[number].control[HBP] = condition | ((condition & WCR_TYPE_MASK) == WATCHPOINT_BREAK_ON_EXECUTE) && (address != NULL);
}

static void SaveDebugRegisterState(struct WatchContext* context, siginfo_t* information)
{
  int number;

  number = 15;

  while ((number >= 0) &&
         (information->si_addr != context->set.state[number].address))
    number --;

  context->status = number;
}

static void LoadDebugRegisterState(struct WatchContext* context)
{
  int number;

  for (number = 0; number < 15; number ++)
  {
    ptrace(PTRACE_SETHBPREGS, context->process, - (number * 2 + 1), &context->set.state[number].address);
    ptrace(PTRACE_SETHBPREGS, context->process,   (number * 2 + 1), &context->set.state[number].address);
    ptrace(PTRACE_SETHBPREGS, context->process, - (number * 2 + 2), context->set.state[number].control + HWP);
    ptrace(PTRACE_SETHBPREGS, context->process,   (number * 2 + 2), context->set.state[number].control + HBP);
  }
}
#endif

#if defined(__aarch64__)
static void SetDebugRegister(struct WatchContext* context, int number, const void* address, uint32_t condition)
{
  number &= 15;
  context->set.state[HWP].dbg_regs[number].addr = (__u64)address;
  context->set.state[HBP].dbg_regs[number].addr = (__u64)address;
  context->set.state[HWP].dbg_regs[number].ctrl = condition | ((condition & WCR_TYPE_MASK) != WATCHPOINT_BREAK_ON_EXECUTE) && (address != NULL);
  context->set.state[HBP].dbg_regs[number].ctrl = condition | ((condition & WCR_TYPE_MASK) == WATCHPOINT_BREAK_ON_EXECUTE) && (address != NULL);
}

static void SaveDebugRegisterState(struct WatchContext* context, siginfo_t* information)
{
  int number;

  number = 15;

  while ((number >= 0) &&
         ((__u64)information->si_addr != context->set.state[HWP].dbg_regs[number].addr))
    number --;

  context->status = number;
}

static void LoadDebugRegisterState(struct WatchContext* context)
{
  struct iovec vector1;
  struct iovec vector2;

  vector1.iov_base = context->set.state + HWP;
  vector2.iov_base = context->set.state + HBP;
  vector1.iov_len  = sizeof(struct user_hwdebug_state);
  vector2.iov_len  = sizeof(struct user_hwdebug_state);

  ptrace(PTRACE_SETREGSET, context->process, NT_ARM_HW_WATCH, &vector1);
  ptrace(PTRACE_SETREGSET, context->process, NT_ARM_HW_BREAK, &vector2);
}
#endif

// Core routines

static void HandleSignal(int signal)
{
  // Dummy signal handler
}

static int DoWork(void* agrument)
{
  int status;
  int signal;
  siginfo_t information;
  struct WatchContext* context;

  context = (struct WatchContext*)agrument;
  prctl(PR_SET_NAME, "Watcher", NULL, NULL, NULL);

  if (ptrace(PTRACE_ATTACH, context->process, 0, 0) != 0)
  {
    context->error = errno;
    atomic_thread_fence(memory_order_release);
    sem_post(&context->semaphore);
    return EXIT_FAILURE;
  }

  atomic_store_explicit(&context->state, STATE_RUN, memory_order_relaxed);
  sem_post(&context->semaphore);

  while (STATE_RUN)
  {
    waitpid(context->process, &status, 0);
    signal = WSTOPSIG(status);

    if ((WIFEXITED(status) != 0) ||
        (atomic_load_explicit(&context->state, memory_order_relaxed) == STATE_STOP))
    {
      // Exit from the trace only when the tracee is in the STOP state
      break;
    }

    if ((signal == SIGTRAP) &&
        (ptrace(PTRACE_GETSIGINFO, context->process, 0, &information) == 0) &&
        (information.si_code == TRAP_HWBKPT))
    {
      // Save debug status register in case of TRAP_HWBKPT only
      SaveDebugRegisterState(context, &information);
    }

    atomic_thread_fence(memory_order_acq_rel);
    LoadDebugRegisterState(context);

    ptrace(PTRACE_CONT, context->process, 0, signal);
  }

  memset(&context->set, 0, sizeof(struct DebugRegisterState));
  LoadDebugRegisterState(context);

  ptrace(PTRACE_DETACH, context->process, 0, signal);
  atomic_store_explicit(&context->state, STATE_STOP, memory_order_relaxed);
  kill(0, SIGCONT);  // That prevents the process to become a zombie

  return EXIT_SUCCESS;
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct WatchContext* context = NULL;

static void __attribute__((constructor(102))) Initialize()
{
  struct sigaction action;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigaction(SIGNAL, &action, NULL);
}

void TerminateWatch()
{
  pthread_mutex_lock(&lock);

  if (context != NULL)
  {
    if (atomic_exchange_explicit(&context->state, STATE_STOP, memory_order_relaxed) == STATE_RUN)
    {
      kill(context->process, SIGNAL);
      waitpid(context->process, NULL, 0);
    }

    sem_destroy(&context->semaphore);
    munmap(context->stack, STACK_SIZE);
    munmap(context, sizeof(struct WatchContext));
    context = NULL;
  }

  pthread_mutex_unlock(&lock);
}

int SetWatchPoint(int number, const void* address, uint32_t condition)
{
  pthread_mutex_lock(&lock);

  if (context == NULL)
  {
    context = (struct WatchContext*)mmap(NULL, sizeof(struct WatchContext), PROT_WRITE | PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    context->stack   = mmap(NULL, STACK_SIZE, PROT_WRITE | PROT_READ, MAP_PRIVATE | MAP_GROWSDOWN | MAP_ANONYMOUS, -1, 0);
    context->process = getpid();
    context->status  = -1;
    sem_init(&context->semaphore, 1, 0);
    atomic_init(&context->state, 0);
  }

  SetDebugRegister(context, number, address, condition);
  atomic_thread_fence(memory_order_release);

  if (atomic_load_explicit(&context->state, memory_order_relaxed) == STATE_STOP)
  {
    clone(DoWork, context->stack + STACK_SIZE, SIGCHLD, context);
    sem_wait(&context->semaphore);
  }

  kill(context->process, SIGNAL);
  pthread_mutex_unlock(&lock);

  return context->error;
}

int GetWatchPoint()
{
  if ((context == NULL) ||
      (atomic_load_explicit(&context->state, memory_order_acquire) == STATE_STOP))
  {
    // Worker is not running
    return -2;
  }

  return context->status;
}

int MakeWatchPointReport(siginfo_t* information, WatchPointReportFunction report)
{
  int number;

  if ((information->si_signo == SIGTRAP) &&
      (information->si_code  == TRAP_HWBKPT))
  {
    number = GetWatchPoint();
    report(LOG_ERR, "The process has been trapped by Watch Point %i\n", number);
  }

  return 1;
}
