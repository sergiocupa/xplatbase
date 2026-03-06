#include "thread_pool.h"
#include <stdlib.h>

#ifdef XPLATBASE_WIN

#else 
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <limits.h>
#endif

#define POOL_INITIAL_LIST_MAX   400
#define POOL_INITIAL_LIST_COUNT 200
#define POOL_INCREMENT          100

ThreadPool ThrPool = { 0 };


static void thr_list_init(ThreadList* list)
{
	list->Count = 0;
	list->Max   = 100;
	list->Items = (ThreadInfo**)malloc(list->Max * sizeof(ThreadInfo*));
}

static void thr_list_add(ThreadList* list, ThreadInfo* obj)
{
	if (list)
	{
		if (list->Count >= list->Max)
		{
			list->Max   = ((list->Count + sizeof(ThreadInfo)) + list->Max) * 2;
			list->Items = (void**)realloc((void**)list->Items, list->Max * sizeof(ThreadInfo*));
		}

		list->Items[list->Count] = obj;
		list->Count++;
	}
}

static void thr_list_release(ThreadList* list)
{

}

static void task_list_init(TaskList* list)
{
	list->Position = 0;
	list->Max      = 100;
	list->Tasks    = (TaskInstance**)malloc(list->Max * sizeof(TaskInstance*));
}

static void task_list_add(TaskList* list, TaskInstance* obj)
{
	if (list)
	{
		if (list->Position >= list->Max)
		{
			list->Max   = ((list->Position + sizeof(TaskInstance)) + list->Max) * 2;
			list->Tasks = (void**)realloc((void**)list->Tasks, list->Max * sizeof(TaskInstance*));
		}

		list->Tasks[list->Position] = obj;
		list->Position++;
	}
}

static void thr_list_release(TaskList* list)
{

}


// Lista trabalha como fila circular, separado em 3 partes (circular para nao precisar deslocar todos os itens da fila):
//    01. Para uso, onde efetivamente é desenfileirado
//        Fisicamente são duas partes, a regiao da faixa 02 é a aproximação do limite da faixa de trabalho (EndWorkingRange), ou saldo.
//    02. Outra para margem de expansão, onde o monitor fica testando margem de seguranca para decisão de ira expandir ou não
//    03. Por último, faixa para reprocessamento, onde é verificado se tem task que ja foi executado. 
//        As tasks executadas são reaproveitado thread e criada nova e dequeue.
//        Sempre trabalha atraz de Position, para pegar tasks mais antigas.
//        A cada reprocessameto, as tasks recicladas é deslocado EndWorkingRange. 
//        Se não consegue manter range de segurança, a lista é aumentada.
//        Como é uma lista circular, e a cada reciclagem EndWorkingRange é deslocado, e se ele chega ao fim da lista o valor volta ao inicio (0)
//            Position tambem tem este comportamento       


static int task_pooling(PoolingMode mode)
{
	if (mode == POOLING_LIMIT_SEC || mode == POOLING_EXHAUSTED)
	{
		// expandir a lista de threads livres, e tambem a fila principal.

	}

	if (mode != POOLING_NONE)
	{
		int ix = ThrPool.TaskQueue.StartOfBlock;
		while (ix < ThrPool.TaskQueue.Position)
		{


			ix++;
		}


		// Pegar a faixa entre StartOfBlock e Position, e utilizar tasks ociosas.
	    //     Apos pegar as tasks, reposicionar os itens ainda ocupados a direita.
	    // Os itens faltantes, pegar da lista de threads livres.

		
	}
}


static int task_list_monitor(void* args)
{
	ThreadPool* pool = (ThreadPool*)args;
	PoolingMode mode = POOLING_NONE;

	while (ThrPool.Running)
	{
		atomic_set(ThrPool.IsPooling, true);
		mode = POOLING_NONE;

		if (ThrPool.TaskQueue.Position <= ThrPool.TaskQueue.End)
		{
			if (ThrPool.TaskQueue.Position >= ThrPool.TaskQueue.LimitSec)
			{
				mode = POOLING_LIMIT_SEC;
			}
			else if (ThrPool.TaskQueue.Position >= ThrPool.TaskQueue.Limit)
			{
				mode = POOLING_LIMIT;
			}
		}
		else
		{
			mode = POOLING_EXHAUSTED;
		}

		task_pooling(mode);

		atomic_set(ThrPool.IsPooling, false);
		ThrPool.SignalEvent(&ThrPool.PoolingWait);
		ThrPool.WaitEvent(&ThrPool.PoolingEvent);
	}
}


static void trigger_to_evaluate(bool wait_current_pooling_to_finish)
{
	if (!atomic_get(ThrPool.IsPooling))
	{
		ThrPool.SignalEvent(&ThrPool.PoolingEvent);
	}
	else
	{
		ThrPool.WaitEvent(&ThrPool.PoolingWait);
	}
}


static int task_list_dequeue(TaskList* _this, TaskInstance** obj)
{
	int p = atomic_get(_this->Position);
	int e = atomic_get(_this->End);

	if (p <= e)
	{
		*obj = _this->Tasks[p];
		atomic_inc(_this->Position);
		trigger_to_evaluate(false);
	}
	else// Pool exhausted
	{
		trigger_to_evaluate(true);
		*obj = _this->Tasks[_this->Position];
		atomic_inc(_this->Position);
	}
}


// The speed_mode setting is faster, but it consumes more CPU.
void thr_pool_init(bool spin_mode)
{
	if (ThrPool.Initialized) return;
	ThrPool.Initialized = true;

	atomic_initialize(ThrPool.IsPooling, false);

	thr_list_init(&ThrPool.ThrQueue);
	task_list_init(&ThrPool.TaskQueue);

	if (spin_mode)
	{
		ThrPool.WaitEvent      = thread_event_wait_spin;
		ThrPool.SignalEvent    = thread_event_signal_spin;
	}
	else
	{
		ThrPool.WaitEvent      = thread_event_wait;
		ThrPool.SignalEvent    = thread_event_signal;
		ThrPool.SignalAllEvent = thread_event_signal_all;
	}

	ThrPool.Running   = true;
	ThrPool.EventMode = POOLING_NONE;

	int status = thread_create(&ThrPool.MonitorThr, task_list_monitor, &ThrPool);

	// Inicializar thread do monitor
	

	// criar funcoes para criar as listas



	// criar funcoes para alimentar e pegar as listas como fila
	//   fila circular, para economizar percorrer itens
	//   se fila menor que metade do inicio, criar mais threads.
	//   criar faixas para o monitor trabalhar, para evitar colisoes. O monitor trabalha cronologicamente com itens mais antigos.
	//   fila circular nao precisa percorrer a todo momento para encontrar thread disponivel.
	//      monitor verifica o index menor que a posição usada por thr_pool_get_task(). verifica com faixa segura, para evitar colisoes.
	//   O monitor verifica se, a ocupação fica meior que metede, entao cria mais threads, expande a fila de threads, para evitar indisponibilidade e colisoes.
	//      De nao existe thread disponivel, força thr_pool_get_task() esperar. Para evitar isso acontecer, o monitor tem que expandir a tempo.
	//          A expanção tem que ser por fragmentada, pra evitar locks longos.
	//   

	// criar funcao para obter a task

	// apontar intermediaria para monitorar fim da execução da task.
	//    liberar status da thread apos fim, para deixa-la disponivel para posterior reuso.

	// criar funcao para monitorar as listas e filas
}




int thr_pool_get_task(TaskFunc func, void* arg)
{
	TaskInstance* task;
	int status = task_list_dequeue(&ThrPool, &task);

	if (status < 0) return status;

	// Precisa de lock?
	task->Status      = TASK_RUNNING;
	task->Thr->Status = THR_BUSY; // Precisa?
	task->Arg         = arg;
	task->Func        = func;

	task->Func(task->Arg);

	// Precisa de lock?
	task->Status      = TASK_IDLE;
	task->Thr->Status = THR_IDLE;
}