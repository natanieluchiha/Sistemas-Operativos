/* Simplethreads Instructional Thread Package
 * 
 * sthread_user.c - Implements the sthread API using user-level threads.
 *
 *    You need to implement the routines in this file.
 *
 * Change Log:
 * 2002-04-15        rick
 *   - Initial version.
 * 2005-10-12        jccc
 *   - Added semaphores, deleted conditional variables
 */

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <sthread.h>
#include <sthread_user.h>
#include <sthread_ctx.h>
#include <sthread_time_slice.h>

#include "queue.h"

/*Definindo as constantes*/
#define  QUANTUM_BASE 5
#define MAX_PRIORIDADES 15
#define PRIORIDADE_FIXA 4 

struct _sthread {
  sthread_ctx_t *saved_ctx;
  sthread_start_func_t start_routine_ptr;
  long wake_time;
  int join_tid;
  void* join_ret;
  void* args;
  int tid;          /* meramente informativo */

    // >>> Variáveis necessárias para o novo escalonador
    int prioridade_base;     // define o valor fixo da prioridade da thread
    int prioridade_atual;    // muda entre épocas, se for dinâmica
    int quantum;             // tempo restante até precisar trocar de época
    int nice;                // valor definido pelo utilizador
    int prioridade_fixa;     // 1 se for fixa (prioridade 0–4), 0 se for dinâmica (5–14)

    struct _sthread* next;
};


static queue_t *exe_thr_list;         /* lista de threads executaveis */
static queue_t *dead_thr_list;        /* lista de threads "mortas" */
static queue_t *sleep_thr_list;
static queue_t *join_thr_list;
static queue_t *zombie_thr_list;
static struct _sthread *active_thr;   /* thread activa */
static int tid_gen;                   /* gerador de tid's */



#define CLOCK_TICK 100
static long Clock;


/*********************************************************************/
/* Part 1: Creating and Scheduling Threads                           */
/*********************************************************************/

// Fila por prioridade (lista encadeada FIFO)
typedef struct {
    struct _sthread *head;
    struct _sthread *tail;
} sthread_queue_t;

// Runqueue completa (15 filas: 0–14)
typedef struct {
    sthread_queue_t fila[MAX_PRIORIDADES];
} sthread_runqueue_t;

// Escalonador global (ativas + expiradas)
static sthread_runqueue_t *active_rq;
static sthread_runqueue_t *expired_rq;


void insert_in_runquee(sthread_queue_t *fila, struct _sthread *thread) {
    thread->next = NULL;
    if (fila->tail) {
        fila->tail->next = thread;
        fila->tail = thread;
    } else {
        fila->head = fila->tail = thread;
    }
}

void sthread_user_free(struct _sthread *thread);

void sthread_aux_start(void){
  splx(LOW);
  active_thr->start_routine_ptr(active_thr->args);
  sthread_user_exit((void*)0);
}

void sthread_user_dispatcher(void);

void sthread_user_init(void) {

  exe_thr_list = create_queue();
  dead_thr_list = create_queue();
  sleep_thr_list = create_queue();
  join_thr_list = create_queue();
  zombie_thr_list = create_queue();
  tid_gen = 1;

  struct _sthread *main_thread = malloc(sizeof(struct _sthread));
  main_thread->start_routine_ptr = NULL;
  main_thread->args = NULL;
  main_thread->saved_ctx = sthread_new_blank_ctx();
  main_thread->wake_time = 0;
  main_thread->join_tid = 0;
  main_thread->join_ret = NULL;
  main_thread->tid = tid_gen++;

   active_rq = malloc(sizeof(sthread_runqueue_t));
   expired_rq = malloc(sizeof(sthread_runqueue_t));
   for (int i = 0; i < MAX_PRIORIDADES; i++) {
    active_rq->fila[i].head = active_rq->fila[i].tail = NULL;
    expired_rq->fila[i].head = expired_rq->fila[i].tail = NULL;
}
  
  active_thr = main_thread;

  Clock = 1;
  sthread_time_slices_init(sthread_user_dispatcher, CLOCK_TICK);
}


sthread_t sthread_user_create(sthread_start_func_t start_routine, void *arg, int priority) {
    // Validação da prioridade
    if (priority < 0 || priority >= MAX_PRIORIDADES) {
        fprintf(stderr, "Invalid priority: %d (must be between 0 and %d)\n", 
                priority, MAX_PRIORIDADES - 1);
        return NULL;
    }

    // Aloca e inicializa a nova thread
    struct _sthread *new_thread = malloc(sizeof(struct _sthread));
    if (!new_thread) {
        perror("malloc error");
        return NULL;
    }

    // Contexto de arranque (primeiro contexto que chama sthread_aux_start)
    sthread_ctx_start_func_t func = sthread_aux_start;
    new_thread->saved_ctx = sthread_new_ctx(func);

    // Preenche os campos herdados
    new_thread->start_routine_ptr = start_routine;
    new_thread->args             = arg;
    new_thread->wake_time        = 0;
    new_thread->join_tid         = 0;
    new_thread->join_ret         = NULL;

    // Atribui o TID
    new_thread->tid = tid_gen++;

    // >>> Preenche os campos do novo escalonador
    new_thread->prioridade_base  = priority;
    new_thread->prioridade_atual = priority;
    new_thread->nice             = 0;
    new_thread->quantum          = QUANTUM_BASE;
    new_thread->prioridade_fixa  = (priority <= PRIORIDADE_FIXA) ? 1 : 0;
    new_thread->next             = NULL;

    // Insere na runqueue ativa correspondente
    // (assumindo que active_rq já foi inicializado em sthread_user_init)
    insert_in_runquee(&active_rq->fila[priority], new_thread);
  splx(HIGH);
  new_thread->tid = tid_gen++;
  queue_insert(exe_thr_list, new_thread);
  splx(LOW);
  return new_thread;


    return new_thread;
}


int sthread_nice(int new_nice){
   if(new_nice < 0 || new_nice > 10){
      fprintf(stderr,"ERROR:Nice value(%d) invalid. The nice value should be between 0 and 10\n", new_nice);
      return -1;
   }

   struct _sthread *t = active_thr;

   if (t->prioridade_fixa) {
       return t->prioridade_atual;
   }

   t->nice = new_nice;

   int nova_prioridade = t->prioridade_base + t->nice;

   if (nova_prioridade < 5) nova_prioridade = 5;
   if (nova_prioridade > 14) nova_prioridade = 14;

   return nova_prioridade;
}


void sthread_user_dump() {
    printf("=== dump start ===\n");

    // Thread ativa
    if (active_thr != NULL) {
        printf("active thread\n");
        printf("id: %d\n", active_thr->tid);
        printf("priority: %d\n", active_thr->prioridade_atual);
        printf("quantum: %d\n", active_thr->quantum);
    }

    // Runqueue ativa
    printf("active runqueue\n");
    for (int i = 0; i < MAX_PRIORIDADES; i++) {
        printf("[%d]", i);
        struct _sthread *t = active_rq->fila[i].head;
        while (t) {
            printf(" %d,%d", t->tid, t->quantum);
            t = t->next;
        }
        printf("\n");
    }

    // Runqueue expirada
    printf("expired runqueue\n");
    for (int i = 0; i < MAX_PRIORIDADES; i++) {
        printf("[%d]", i);
        struct _sthread *t = expired_rq->fila[i].head;
        while (t) {
            printf(" %d,%d", t->tid, t->quantum);
            t = t->next;
        }
        printf("\n");
    }

    // Lista de bloqueadas (sleep_thr_list + threads em monitores)
    printf("blocked list\n");

    // Lista de espera por "sleep"
    queue_element_t *qe = sleep_thr_list->first;
    while (qe) {
        printf("%d,%d ", qe->thread->tid, qe->thread->quantum);
        qe = qe->next;
    }

    // (Opcional) Lista de espera por monitor — se quiser manter um queue global
    // para bloqueados por monitor, adicione aqui também

    printf("\n=== dump end ===\n");
}





void sthread_user_exit(void *ret) {
  splx(HIGH);
   
   int is_zombie = 1;

   // unblock threads waiting in the join list
   queue_t *tmp_queue = create_queue();   
   while (!queue_is_empty(join_thr_list)) {
      struct _sthread *thread = queue_remove(join_thr_list);
     
      printf("Test join list: join_tid=%d, active->tid=%d\n", thread->join_tid, active_thr->tid);
      if (thread->join_tid == active_thr->tid) {
         thread->join_ret = ret;
         queue_insert(exe_thr_list,thread);
         is_zombie = 0;
      } else {
         queue_insert(tmp_queue,thread);
      }
   }
   delete_queue(join_thr_list);
   join_thr_list = tmp_queue;
 
   if (is_zombie) {
      queue_insert(zombie_thr_list, active_thr);
   } else {
      queue_insert(dead_thr_list, active_thr);
   }
   

   if(queue_is_empty(exe_thr_list)){  /* pode acontecer se a unica thread em execucao fizer */
    free(exe_thr_list);              /* sthread_exit(0). Este codigo garante que o programa sai bem. */
    delete_queue(dead_thr_list);
    sthread_user_free(active_thr);
    printf("Exec queue is empty!\n");
    exit(0);
  }

  
   // remove from exec list
   struct _sthread *old_thr = active_thr;
   active_thr = queue_remove(exe_thr_list);
   sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);

   splx(LOW);
}


void sthread_user_dispatcher(void)
{
   Clock++;

   queue_t *tmp_queue = create_queue();   

   while (!queue_is_empty(sleep_thr_list)) {
      struct _sthread *thread = queue_remove(sleep_thr_list);
      
      if (thread->wake_time == Clock) {
         thread->wake_time = 0;
         queue_insert(exe_thr_list,thread);
      } else {
         queue_insert(tmp_queue,thread);
      }
   }
   delete_queue(sleep_thr_list);
   sleep_thr_list = tmp_queue;
   
   sthread_user_yield();
}


void sthread_user_yield(void)
{
  splx(HIGH);
  struct _sthread *old_thr;
  old_thr = active_thr;
  queue_insert(exe_thr_list, old_thr);
  active_thr = queue_remove(exe_thr_list);
  sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
  splx(LOW);
}



void sthread_user_free(struct _sthread *thread)
{
  sthread_free_ctx(thread->saved_ctx);
  free(thread);
}


/*********************************************************************/
/* Part 2: Join and Sleep Primitives                                 */
/*********************************************************************/

int sthread_user_join(sthread_t thread, void **value_ptr)
{
   /* suspends execution of the calling thread until the target thread
      terminates, unless the target thread has already terminated.
      On return from a successful pthread_join() call with a non-NULL 
      value_ptr argument, the value passed to pthread_exit() by the 
      terminating thread is made available in the location referenced 
      by value_ptr. When a pthread_join() returns successfully, the 
      target thread has been terminated. The results of multiple 
      simultaneous calls to pthread_join() specifying the same target 
      thread are undefined. If the thread calling pthread_join() is 
      canceled, then the target thread will not be detached. 

      If successful, the pthread_join() function returns zero. 
      Otherwise, an error number is returned to indicate the error. */

   
   splx(HIGH);
   // checks if the thread to wait is zombie
   int found = 0;
   queue_t *tmp_queue = create_queue();
   while (!queue_is_empty(zombie_thr_list)) {
      struct _sthread *zthread = queue_remove(zombie_thr_list);
      if (thread->tid == zthread->tid) {
         *value_ptr = thread->join_ret;
         queue_insert(dead_thr_list,thread);
         found = 1;
      } else {
         queue_insert(tmp_queue,zthread);
      }
   }
   delete_queue(zombie_thr_list);
   zombie_thr_list = tmp_queue;
  
   if (found) {
       splx(LOW);
       return 0;
   }

   
   // search active queue
   if (active_thr->tid == thread->tid) {
      found = 1;
   }
   
   queue_element_t *qe = NULL;

   // search exe
   qe = exe_thr_list->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         printf("Found in exe: tid=%d\n", thread->tid);
         found = 1;
      }
      qe = qe->next;
   }

   // search sleep
   qe = sleep_thr_list->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         found = 1;
      }
      qe = qe->next;
   }

   // search join
   qe = join_thr_list->first;
   while (!found && qe != NULL) {
      if (qe->thread->tid == thread->tid) {
         found = 1;
      }
      qe = qe->next;
   }

   // if found blocks until thread ends
   if (!found) {
      splx(LOW);
      return -1;
   } else {
      active_thr->join_tid = thread->tid;
      
      struct _sthread *old_thr = active_thr;
      queue_insert(join_thr_list, old_thr);
      active_thr = queue_remove(exe_thr_list);
      printf ("Active is 0:%d\n", (active_thr == NULL));
      printf ("Old is 0:%d\n", (old_thr == NULL));
      sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
  
      *value_ptr = thread->join_ret;
   }
   
   splx(LOW);
   return 0;
}


int sthread_user_sleep(int time)
{
   splx(HIGH);
   
   long num_ticks = 10 * time / CLOCK_TICK;
   if (num_ticks == 0) {
      splx(LOW);
      
      return 0;
   }
   
   active_thr->wake_time = Clock + num_ticks;

   queue_insert(sleep_thr_list,active_thr); 
   sthread_t old_thr = active_thr;
   active_thr = queue_remove(exe_thr_list);
   sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
   
   splx(LOW);
   return 0;
}

/* --------------------------------------------------------------------------*
 * Synchronization Primitives                                                *
 * ------------------------------------------------------------------------- */

/*
 * Mutex implementation
 */

struct _sthread_mutex
{
  lock_t l;
  struct _sthread *thr;
  queue_t* queue;
};

sthread_mutex_t sthread_user_mutex_init()
{
  sthread_mutex_t lock;

  if(!(lock = malloc(sizeof(struct _sthread_mutex)))){
    printf("Error in creating mutex\n");
    return 0;
  }

  /* mutex initialization */
  lock->l=0;
  lock->thr = NULL;
  lock->queue = create_queue();
  
  return lock;
}

void sthread_user_mutex_free(sthread_mutex_t lock)
{
  delete_queue(lock->queue);
  free(lock);
}

void sthread_user_mutex_lock(sthread_mutex_t lock)
{
  while(atomic_test_and_set(&(lock->l))) {}

  if(lock->thr == NULL){
    lock->thr = active_thr;

    atomic_clear(&(lock->l));
  } else {
    queue_insert(lock->queue, active_thr);
    
    atomic_clear(&(lock->l));

    splx(HIGH);
    struct _sthread *old_thr;
    old_thr = active_thr;
    //queue_insert(exe_thr_list, old_thr);
    active_thr = queue_remove(exe_thr_list);
    sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);

    splx(LOW);
  }
}

void sthread_user_mutex_unlock(sthread_mutex_t lock)
{
  if(lock->thr!=active_thr){
    printf("unlock without lock!\n");
    return;
  }

  while(atomic_test_and_set(&(lock->l))) {}

  if(queue_is_empty(lock->queue)){
    lock->thr = NULL;
  } else {
    lock->thr = queue_remove(lock->queue);
    queue_insert(exe_thr_list, lock->thr);
  }

  atomic_clear(&(lock->l));
}

/*
 * Monitor implementation
 */

struct _sthread_mon {
 	sthread_mutex_t mutex;
	queue_t* queue;
};

sthread_mon_t sthread_user_monitor_init()
{
  sthread_mon_t mon;
  if(!(mon = malloc(sizeof(struct _sthread_mon)))){
    printf("Error creating monitor\n");
    return 0;
  }

  mon->mutex = sthread_user_mutex_init();
  mon->queue = create_queue();
  return mon;
}

void sthread_user_monitor_free(sthread_mon_t mon)
{
  sthread_user_mutex_free(mon->mutex);
  delete_queue(mon->queue);
  free(mon);
}

void sthread_user_monitor_enter(sthread_mon_t mon)
{
  sthread_user_mutex_lock(mon->mutex);
}

void sthread_user_monitor_exit(sthread_mon_t mon)
{
  sthread_user_mutex_unlock(mon->mutex);
}

void sthread_user_monitor_wait(sthread_mon_t mon)
{
  struct _sthread *temp;

  if(mon->mutex->thr != active_thr){
    printf("monitor wait called outside monitor\n");
    return;
  }

  /* inserts thread in queue of blocked threads */
  temp = active_thr;
  queue_insert(mon->queue, temp);

  /* exits mutual exclusion region */
  sthread_user_mutex_unlock(mon->mutex);

	splx(HIGH);
	struct _sthread *old_thr;
	old_thr = active_thr;
	//queue_insert(exe_thr_list, old_thr);
	active_thr = queue_remove(exe_thr_list);
	sthread_switch(old_thr->saved_ctx, active_thr->saved_ctx);
	splx(LOW);
}

void sthread_user_monitor_signal(sthread_mon_t mon)
{
  struct _sthread *temp;

  if(mon->mutex->thr != active_thr){
    printf("monitor signal called outside monitor\n");
    return;
  }

  while(atomic_test_and_set(&(mon->mutex->l))) {}
  if(!queue_is_empty(mon->queue)){
    /* changes blocking queue for thread */
    temp = queue_remove(mon->queue);
    queue_insert(mon->mutex->queue, temp);
  }
  atomic_clear(&(mon->mutex->l));
}




/* The following functions are dummies to 
 * highlight the fact that pthreads do not
 * include monitors.
 */

sthread_mon_t sthread_dummy_monitor_init()
{
   printf("WARNING: pthreads do not include monitors!\n");
   return NULL;
}


void sthread_dummy_monitor_free(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_enter(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_exit(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_wait(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}


void sthread_dummy_monitor_signal(sthread_mon_t mon)
{
   printf("WARNING: pthreads do not include monitors!\n");
}
int sthread_get_tid(struct _sthread *thr) {
    return thr->tid;
}



