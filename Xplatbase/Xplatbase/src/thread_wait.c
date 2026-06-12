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
/*  Windows - WaitOnAddress / WakeByAddressSingle (equivalente ao futex)   */
/* ======================================================================== */

#ifdef _WIN32

#include <mmsystem.h>
#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "Winmm.lib")

static SRWLOCK g_timer_lock = SRWLOCK_INIT;
static unsigned int g_timer_users = 0;

/* Init - eleva resolucao do timer do Windows para 1ms (afeta Sleep/Wait*).
 * Por padrao e ~15.6ms — sem isso, Sleep(1) dorme ate 15ms e
 * WaitOnAddress(timeout=1ms) tambem espera ate 15ms reais. */
boolean thread_wait_init(boolean fast_mode)
{
    (void)fast_mode;
    boolean ok = true;
    AcquireSRWLockExclusive(&g_timer_lock);
    if (g_timer_users == 0 && timeBeginPeriod(1) != TIMERR_NOERROR) {
        ok = false;
    } else {
        g_timer_users++;
    }
    ReleaseSRWLockExclusive(&g_timer_lock);
    return ok;
}

void thread_wait_shutdown(void)
{
    AcquireSRWLockExclusive(&g_timer_lock);
    if (g_timer_users > 0 && --g_timer_users == 0)
        timeEndPeriod(1);
    ReleaseSRWLockExclusive(&g_timer_lock);
}

/* Reseta o sinal para 0 antes de entrar na fase de espera. */
void thread_wait_prepare(xwait_t* w)
{
    atomic_set(&w->signal, 0);
}

/* Dorme ate ser acordada (sem timeout). */
void thread_wait_sleep(xwait_t* w)
{
    LONG expected = 0;
    while (atomic_get(&w->signal) == 0)
        WaitOnAddress(&w->signal, &expected, sizeof(LONG), INFINITE);
    atomic_set(&w->signal, 0);
}

/* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout.
 * WaitOnAddress: dorme apenas se *address == *compare no momento da chamada,
 * portanto nao ha race entre wake-antes-de-sleep. */
boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us)
{
    LONG  expected = 0;
    DWORD ms       = (DWORD)(timeout_us / 1000);
    if (ms == 0) ms = 1;

    BOOL woken = WaitOnAddress(&w->signal, &expected, sizeof(LONG), ms);
    atomic_set(&w->signal, 0);
    return woken != FALSE;
}

/* Acorda a thread imediatamente. */
void thread_wait_wake(xwait_t* w)
{
    atomic_set(&w->signal, 1);
    WakeByAddressSingle(&w->signal);
}

/* No-op: sem handle para fechar. */
void thread_wait_destroy(xwait_t* w)
{
    (void)w;
}

/* ======================================================================== */
/*  Linux - futex syscall direta                                            */
/* ======================================================================== */

#elif defined(__linux__)

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

/* Init - futex nao precisa */
boolean thread_wait_init(boolean fast_mode)
{
    (void)fast_mode;
    return true;
}

void thread_wait_shutdown(void)
{
}

/* Libera recursos da instancia (no-op no Linux) */
void thread_wait_destroy(xwait_t* w)
{
    (void)w;
}

/* Registrar thread antes de usar */
void thread_wait_prepare(xwait_t* w)
{
    atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
}

/* Dorme ate ser acordada - loop por causa de spurious wakeup (EINTR, etc) */
void thread_wait_sleep(xwait_t* w)
{
    while (atomic_load_explicit(&w->futex_val, memory_order_acquire) == 0)
        syscall(SYS_futex, &w->futex_val, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
            0, NULL, NULL, 0);

    atomic_store_explicit(&w->futex_val, 0, memory_order_relaxed);
}

/* Dorme com timeout (microsegundos). true = acordou por wake, false = timeout */
boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us)
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
void thread_wait_wake(xwait_t* w)
{
    atomic_store_explicit(&w->futex_val, 1, memory_order_release);
    syscall(SYS_futex, &w->futex_val, FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
        1, NULL, NULL, 0);
}

#else
#error "Plataforma nao suportada. Requer Windows ou Linux."
#endif



