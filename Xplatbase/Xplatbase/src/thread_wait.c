#include "thread_wait.h"
#include <stdbool.h>

/* ======================================================================== */
/*  CPU pause/yield                                                         */
/* ======================================================================== */

#if defined(_WIN32)
#include <intrin.h>
#define xwait_cpu_pause()  _mm_pause()
#elif defined(__x86_64__) || defined(__i386__)
#define xwait_cpu_pause()  __asm__ __volatile__("pause" ::: "memory")
#elif defined(__aarch64__)
#define xwait_cpu_pause()  __asm__ __volatile__("yield" ::: "memory")
#elif defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
#define xwait_cpu_pause()  __asm__ __volatile__("yield" ::: "memory")
#else
#define xwait_cpu_pause()  __asm__ __volatile__("" ::: "memory")
#endif

/* ======================================================================== */
/*  Windows - NT direct syscalls                                            */
/* ======================================================================== */

#ifdef _WIN32

//#define WIN32_LEAN_AND_MEAN
//#include <windows.h>

typedef long NTSTATUS;
#define XWAIT_STATUS_TIMEOUT  ((NTSTATUS)0x00000102)

typedef NTSTATUS(NTAPI* fn_NtWaitForAlertByThreadId)(PVOID, PLARGE_INTEGER);
typedef NTSTATUS(NTAPI* fn_NtAlertThreadByThreadId)(HANDLE);

static fn_NtWaitForAlertByThreadId  xwait__wait_fn;
static fn_NtAlertThreadByThreadId   xwait__alert_fn;

static boolean SpinMode;


/* Init - chamar uma vez */
boolean thread_wait_init(boolean fast_mode)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;

	SpinMode = fast_mode;

    xwait__wait_fn  = (fn_NtWaitForAlertByThreadId)GetProcAddress(ntdll, "NtWaitForAlertByThreadId");
    xwait__alert_fn = (fn_NtAlertThreadByThreadId)GetProcAddress(ntdll, "NtAlertThreadByThreadId");

    return (xwait__wait_fn != NULL && xwait__alert_fn != NULL);
}

/* Registrar thread antes de usar */
void thread_wait_prepare(xwait_t* w)
{
    w->thread_id = GetCurrentThreadId();
}


static inline void xwait_spin_sleep(xwait_t* w)
{
    while (atomic_get(&w->signal) == 0)
    {
        xwait_cpu_pause();
    }

    atomic_set(&w->signal, 0);
}

static inline void xwait_spin_wake(xwait_t* w)
{
    atomic_set(&w->signal, 1);
}


/* Dorme ate ser acordada - sem loop, NT nao tem spurious wakeup */
void thread_wait_sleep(xwait_t* w)
{
    if (SpinMode)
    {
        xwait_spin_sleep(w);
    }
    else
    {
        (void)w;
        xwait__wait_fn(NULL, NULL);
    }
}

/* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout */
boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us)
{
    (void)w;
    LARGE_INTEGER li;
    li.QuadPart = -(timeout_us * 10LL);

    NTSTATUS status = xwait__wait_fn(NULL, &li);
    return (status != XWAIT_STATUS_TIMEOUT);
}

/* Acorda a thread */
void thread_wait_wake(xwait_t* w)
{
    if (SpinMode)
    {
        xwait_spin_wake(w);
    }
    else
    {
        xwait__alert_fn((HANDLE)(ULONG_PTR)w->thread_id);
    }
}

/* ======================================================================== */
/*  Linux - futex syscall direta                                            */
/* ======================================================================== */

#elif 

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* Init - futex nao precisa */
inline boolean thread_wait_init(void)
{
    return true;
}

/* Registrar thread antes de usar */
inline void thread_wait_prepare(xwait_t* w)
{
    atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
}

/* Dorme ate ser acordada - loop por causa de spurious wakeup (EINTR, etc) */
inline void thread_wait_sleep(xwait_t* w)
{
    while (atomic_load_explicit(&w->futex_val, memory_order_acquire) == 0)
        syscall(SYS_futex, &w->futex_val, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            0, NULL, NULL, 0);

    atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
}

/* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout */
inline boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us)
{
    struct timespec ts;
    ts.tv_sec = timeout_us / 1000000LL;
    ts.tv_nsec = (timeout_us % 1000000LL) * 1000LL;

    while (atomic_load_explicit(&w->futex_val, memory_order_acquire) == 0) {
        long ret = syscall(SYS_futex, &w->futex_val, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            0, &ts, NULL, 0);

        if (ret == -1 && errno == ETIMEDOUT)
            return false;
    }

    atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
    return true;
}

/* Acorda a thread */
inline void thread_wait_wake(xwait_t* w)
{
    atomic_store_explicit(&w->futex_val, 1, memory_order_release);
    syscall(SYS_futex, &w->futex_val, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        1, NULL, NULL, 0);
}

#else
#error "Plataforma nao suportada. Requer Windows ou Linux."
#endif



