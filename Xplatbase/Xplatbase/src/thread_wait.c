#include "thread_wait.h"
#include <stdbool.h>


/* ======================================================================== */
/*  Windows - resolucao do timer do sistema (estado global de refcount)    */
/* ======================================================================== */

#ifdef _WIN32

#include <mmsystem.h>
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

/* ======================================================================== */
/*  Linux - futex nao precisa de estado de inicializacao                    */
/* ======================================================================== */

#elif defined(__linux__)

boolean thread_wait_init(boolean fast_mode)
{
    (void)fast_mode;
    return true;
}

void thread_wait_shutdown(void)
{
}

#else
#error "Plataforma nao suportada. Requer Windows ou Linux."
#endif



/* Definicao unica (linkage externa) das versoes exportadas -- so encaminham
 * para a variante _inline. Uso interno da lib deve chamar direto os _inline. */

void thread_wait_destroy(xwait_t* w)
{
    thread_wait_destroy_inline(w);
}

void thread_wait_prepare(xwait_t* w)
{
    thread_wait_prepare_inline(w);
}

void thread_wait_sleep(xwait_t* w)
{
    thread_wait_sleep_inline(w);
}

boolean thread_wait_sleep_for(xwait_t* w, long long timeout_us)
{
    return thread_wait_sleep_for_inline(w, timeout_us);
}

void thread_wait_wake(xwait_t* w)
{
    thread_wait_wake_inline(w);
}

void thread_cond_init(xcond_t* cond)
{
    thread_cond_init_inline(cond);
}

void thread_cond_signal(xcond_t* cond)
{
    thread_cond_signal_inline(cond);
}

void thread_cond_wait(xcond_t* cond, xmutex_t* mutex)
{
    thread_cond_wait_inline(cond, mutex);
}

void thread_cond_destroy(xcond_t* cond)
{
    thread_cond_destroy_inline(cond);
}
