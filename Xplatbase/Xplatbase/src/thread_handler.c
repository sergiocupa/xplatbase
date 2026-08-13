#include "thread_handler.h"
#include <stdlib.h>

#define THR_LIST_INIT_COUNT 16

static ThreadList Threads = { 0 };
static CreatedThread OnCreatedThread = 0;
static CreatedThread OnEndedThread   = 0;
static int ThreadHandlerInited = 0;

static void create_list()
{
	Threads.Max   = THR_LIST_INIT_COUNT;
    Threads.Count = 0;
	Threads.Items = (Thread**)malloc(sizeof(Thread*) * Threads.Max);
}

static void add_list(Thread* thr)
{
    int sz = Threads.Count + 1;
    if (sz >= Threads.Max)
    {
        uint64 new_max = (uint64)Threads.Max * 2;
        void** new_items = (void**)realloc(Threads.Items, new_max * sizeof(void*));
        Threads.Items = new_items;
        Threads.Max = (uint64)new_max;
    }
    Threads.Items[Threads.Count] = thr;
    Threads.Count++;
}

static void remove_list(Thread* thr)
{
    int ix = 0;
    while (ix < Threads.Count)
    {
        void* am = Threads.Items[ix];
        if (am == thr)
        {
            // transfere instancias
            while (ix < Threads.Count - 1)
            {
                Threads.Items[ix] = Threads.Items[ix + 1];
                ix++;
            }
            Threads.Count--;
            break;
        }
        ix++;
    }
}


void xpb_list_init_ext(ThreadList* list, int32 initial_count, uint64 type_size, const char* func, const char* file, int line);
void xpb_list_add_ext(ThreadList* list, void* instance, uint64 type_size, const char* func, const char* file, int line);
void xpb_list_remove_ext(ThreadList* list, void* obj);

#ifdef XPLATBASE_WIN
static CRITICAL_SECTION ThreadsLock;
static INIT_ONCE ThreadsLockOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK thread_lock_init_once(PINIT_ONCE once, PVOID param, PVOID* context)
{
    (void)once;
    (void)param;
    (void)context;
    InitializeCriticalSection(&ThreadsLock);
    return TRUE;
}

static void thread_lock_init(void)
{
    InitOnceExecuteOnce(&ThreadsLockOnce, thread_lock_init_once, NULL, NULL);
}

static void thread_lock(void)
{
    thread_lock_init();
    EnterCriticalSection(&ThreadsLock);
}

static void thread_unlock(void)
{
    LeaveCriticalSection(&ThreadsLock);
}
#else
static pthread_mutex_t ThreadsLock = PTHREAD_MUTEX_INITIALIZER;

static void thread_lock_init(void)
{
}

static void thread_lock(void)
{
    pthread_mutex_lock(&ThreadsLock);
}

static void thread_unlock(void)
{
    pthread_mutex_unlock(&ThreadsLock);
}
#endif

static void thread_handler_init_once(void)
{
    thread_lock_init();
    thread_lock();
    if (!ThreadHandlerInited)
    {
        create_list();
        ThreadHandlerInited = 1;
    }
    thread_unlock();
}

#ifdef XPLATBASE_WIN
static DWORD WINAPI tfun(void* arg)
#else
static void* tfun(void* arg)
#endif
{
    Thread* ta = (Thread*)arg;
    CreatedThread on_created;
    CreatedThread on_ended;
    xthread_result_t result = 0;

    thread_lock();
    on_created = OnCreatedThread;
    on_ended   = OnEndedThread;
    thread_unlock();

    if (on_created) on_created(ta);

    result = ta->Func.func(ta->Func.arg);

    if (on_ended) on_ended(ta);

    thread_lock();
    remove_list(ta);
    thread_unlock();

#ifdef XPLATBASE_WIN
    (void)result;
    return 0;
#else
    return result;
#endif
}

Thread* thread_create(xthread_func_t* func, void* arg, int* status)
{
    int ok = 0;
    Thread* thr;

    if (status) *status = 0;
    if (!func) return NULL;

    thr = (Thread*)malloc(sizeof(Thread));
    if (!thr) return NULL;

    thr->Func.func = func;
    thr->Func.arg = arg;
    thr->Joined = 0;

    /* garante a lista inicializada mesmo sem thread_init()/platform_init()
     * (ex.: link sem o auto-init do CRT, ou LTCG que o descarta) */
    thread_handler_init_once();

    thread_lock();
    add_list(thr);
    thread_unlock();

#ifdef XPLATBASE_WIN
    thr->Thr = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)tfun, thr, 0, NULL);
    ok = (thr->Thr != NULL) ? 1 : 0;
#else
    ok = pthread_create(&thr->Thr, NULL, tfun, thr) == 0;
#endif

    if (!ok)
    {
        thread_lock();
        remove_list(thr);
        thread_unlock();
        free(thr);
        return NULL;
    }

    if (status) *status = 1;
    return thr;
}

void thread_join(Thread** t)
{
    Thread* thr;

    if (!t || !*t) return;
    thr = *t;

    if (!thr->Joined)
    {
#ifdef XPLATBASE_WIN
        WaitForSingleObject(thr->Thr, INFINITE);
        CloseHandle(thr->Thr);
#else
        pthread_join(thr->Thr, NULL);
#endif
        thr->Joined = 1;
    }

    free(thr);
    *t = NULL;
}

void thread_init(CreatedThread created_thread, CreatedThread ended_thread)
{
    thread_handler_init_once();

    thread_lock();
    OnCreatedThread = created_thread;
    OnEndedThread   = ended_thread;
    thread_unlock();
}

void thread_enum(ThreadEnumCb cb, void* ctx)
{
    int i;
    if (!cb) return;

    thread_handler_init_once();

    thread_lock();
    for (i = 0; i < (int)Threads.Count; i++)
        cb((const Thread*)Threads.Items[i], ctx);
    thread_unlock();
}



/* Definicao unica (linkage externa) das versoes exportadas -- so encaminham
 * para a variante _inline. Uso interno da lib deve chamar direto os _inline. */

void thread_mutex_init(xmutex_t* mutex)
{
    thread_mutex_init_inline(mutex);
}

void thread_mutex_lock(xmutex_t* mutex)
{
    thread_mutex_lock_inline(mutex);
}

void thread_mutex_unlock(xmutex_t* mutex)
{
    thread_mutex_unlock_inline(mutex);
}

void thread_mutex_destroy(xmutex_t* mutex)
{
    thread_mutex_destroy_inline(mutex);
}

void thread_yield(void)
{
    thread_yield_inline();
}

void thread_sleep0(void)
{
    thread_sleep0_inline();
}

long long thread_atomic64_load(xthread_atomic64* p)
{
    return thread_atomic64_load_inline(p);
}

void thread_atomic64_store(xthread_atomic64* p, long long v)
{
    thread_atomic64_store_inline(p, v);
}

void thread_fence(void)
{
    thread_fence_inline();
}

int thread_atomic64_cas(xthread_atomic64* p, long long expected, long long desired)
{
    return thread_atomic64_cas_inline(p, expected, desired);
}
