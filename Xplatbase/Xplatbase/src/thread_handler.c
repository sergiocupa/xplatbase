#include "thread_handler.h"
#include <stdlib.h>

static ListXPB Threads = { 0 };
static CreatedThread OnCreatedThread = 0;
static CreatedThread OnEndedThread   = 0;
static int ThreadHandlerInited = 0;

void xpb_list_init_ext(ListXPB* list, int32 initial_count, uint64 type_size, const char* func, const char* file, int line);
void xpb_list_add_ext(ListXPB* list, void* instance, uint64 type_size, const char* func, const char* file, int line);
void xpb_list_remove_ext(ListXPB* list, void* obj);

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
        xpb_list_init(&Threads, -1, Thread);
        Threads.Active = true;
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
    on_ended = OnEndedThread;
    thread_unlock();

    if (on_created) on_created(ta);

    result = ta->Func.func(ta->Func.arg);

    if (on_ended) on_ended(ta);

    thread_lock();
    xpb_list_remove(&Threads, ta);
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

    thread_lock();
    xpb_list_add(&Threads, thr, Thread);
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
        xpb_list_remove(&Threads, thr);
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
