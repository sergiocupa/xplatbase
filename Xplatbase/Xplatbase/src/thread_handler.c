#include "../include/xplatbase.h"



int thread_create(void** t, void* func, void* arg)
{
#ifdef XPLATBASE_WIN
    *t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
    return (*t != NULL) ? 1 : 0;
#else
    return pthread_create(t, NULL, (thread_func_t)func, arg) == 0;
#endif
}


void thread_join(void** t)
{
    #ifdef _WIN32
        WaitForSingleObject(*t, INFINITE);
        CloseHandle(*t);
    #else
        pthread_join(*t, NULL);
    #endif
}