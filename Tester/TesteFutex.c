// ========================================================
//  Teste de Latencia - NtWaitForAlertByThreadId (nativo)
//  Dois modos: com spin e sem spin (~10s cada)
//  Para Xeon E5-2640 v3
// ========================================================

#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <immintrin.h>

#pragma comment(lib, "ntdll.lib")

#define TEST_DURATION_SEC  10
#define WARMUP             2000
#define SPIN_COUNT         1200
#define MAX_SAMPLES        2000000

typedef NTSTATUS(NTAPI* pNtWaitForAlertByThreadId)(
    _In_opt_ PVOID Address,
    _In_opt_ PLARGE_INTEGER Timeout
    );

typedef NTSTATUS(NTAPI* pNtAlertThreadByThreadId)(
    _In_ HANDLE ThreadId
    );

pNtWaitForAlertByThreadId  NtWaitForAlertByThreadId = NULL;
pNtAlertThreadByThreadId   NtAlertThreadByThreadId = NULL;

volatile uint32_t g_flag = 0;
volatile uint64_t g_signal_time = 0;
volatile int      g_running = 1;
LARGE_INTEGER g_freq;

int g_use_spin = 1;

uint64_t get_ns(void) 
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / g_freq.QuadPart);
}

int cmp_u64(const void* a, const void* b) 
{
    uint64_t va = *(const uint64_t*)a;
    uint64_t vb = *(const uint64_t*)b;
    return (va > vb) - (va < vb);
}

void wait_ntalert(void) 
{
    if (g_use_spin) {
        for (int i = 0; i < SPIN_COUNT; i++) {
            if (g_flag != 0) return;
            _mm_pause();
        }
    }
    while (g_flag == 0) {
        NtWaitForAlertByThreadId(NULL, NULL);
    }
}

void signal_ntalert(HANDLE threadId) 
{
    g_flag = 1;
    NtAlertThreadByThreadId(threadId);
}

// ====================== THREAD PRODUTORA ======================
DWORD WINAPI producer(LPVOID param) 
{
    HANDLE consThreadId = (HANDLE)param;

    while (g_running) 
    {
        while (g_flag != 0 && g_running) _mm_pause();
        if (!g_running) break;

        g_signal_time = get_ns();
        signal_ntalert(consThreadId);
    }
    return 0;
}

// ====================== TESTE ======================
void run_test(const char* name, int consumer_core, int producer_core) 
{
    printf("\n=== %s | Core %d -> %d ===\n", name, consumer_core, producer_core);
    printf("Rodando por %d segundos...\n", TEST_DURATION_SEC);

    uint64_t* latencies = (uint64_t*)malloc(MAX_SAMPLES * sizeof(uint64_t));
    if (!latencies) { printf("Erro malloc\n"); return; }

    g_flag = 0;
    g_running = 1;

    HANDLE consumerThreadId = (HANDLE)(ULONG_PTR)GetCurrentThreadId();

    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1ULL << consumer_core);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HANDLE hProducer = CreateThread(NULL, 0, producer, (LPVOID)consumerThreadId, 0, NULL);
    SetThreadAffinityMask(hProducer, (DWORD_PTR)1ULL << producer_core);
    SetThreadPriority(hProducer, THREAD_PRIORITY_TIME_CRITICAL);

    uint64_t t_start = get_ns();
    uint64_t t_end = t_start + (uint64_t)TEST_DURATION_SEC * 1000000000ULL;
    int total = 0;
    int idx = 0;

    while (get_ns() < t_end) 
    {
        g_flag = 0;
        wait_ntalert();
        total++;

        if (total > WARMUP && idx < MAX_SAMPLES) 
        {
            latencies[idx++] = get_ns() - g_signal_time;
        }
    }

    g_running = 0;
    g_flag = 0;
    Sleep(1);

    WaitForSingleObject(hProducer, INFINITE);
    CloseHandle(hProducer);

    int n = idx;
    printf("Iteracoes totais: %d (amostras: %d)\n", total, n);

    if (n == 0) { free(latencies); return; }

    qsort(latencies, n, sizeof(uint64_t), cmp_u64);

    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += latencies[i];

    printf("Latencia de wake (ns):\n");
    printf("   Minimo   : %8llu\n", (unsigned long long)latencies[0]);
    printf("   Media    : %8.1f\n", (double)sum / n);
    printf("   Mediana  : %8llu\n", (unsigned long long)latencies[n / 2]);
    printf("   P99      : %8llu\n", (unsigned long long)latencies[(int)(n * 0.99)]);
    printf("   Maximo   : %8llu\n", (unsigned long long)latencies[n - 1]);

    free(latencies);
}

int __main(void) 
{
    QueryPerformanceFrequency(&g_freq);

    NtWaitForAlertByThreadId = (pNtWaitForAlertByThreadId)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtWaitForAlertByThreadId");
    NtAlertThreadByThreadId  = (pNtAlertThreadByThreadId)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtAlertThreadByThreadId");

    if (!NtWaitForAlertByThreadId || !NtAlertThreadByThreadId) 
    {
        printf("Erro ao carregar NtWaitForAlertByThreadId / NtAlertThreadByThreadId\n");
        system("pause");
        return 1;
    }

    printf("=== Teste NtWaitForAlertByThreadId - Xeon E5-2640 v3 ===\n");
    printf("Mecanismo interno usado pelo WaitOnAddress.\n");
    printf("Cada teste roda por %d segundos.\n", TEST_DURATION_SEC);

    g_use_spin = 1;
    run_test("Spin(1200) + NtAlertThreadByThreadId", 0, 1);

    g_use_spin = 0;
    run_test("NtAlertThreadByThreadId puro (sem spin)", 0, 1);

    printf("\nTeste concluido!\n");
    system("pause");
    return 0;
}