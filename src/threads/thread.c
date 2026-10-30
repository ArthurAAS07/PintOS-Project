#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif
#include "devices/timer.h"

/* ========== MLFQS ========== */

/* Formato 17.14: 17 bits inteiros e 14 bits para a parte fracionaria. */
typedef int32_t fixed_t;

/* Fator de conversao f = 2^14 (16384). */
#define FP_F (1 << 14)

/* Conversoes entre inteiros e ponto fixo. */
#define FP_FROM_INT(n) ((n) * FP_F)
#define FP_TO_INT_ZERO(x) ((x) / FP_F)

/* Converte ponto fixo para inteiro arredondando para o mais proximo. */
#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + FP_F / 2) / FP_F : ((x) - FP_F / 2) / FP_F)

/* Adicao e subtracao em ponto fixo. */
#define FP_ADD(x, y) ((x) + (y))
#define FP_SUB(x, y) ((x) - (y))

/* Adicao e subtracao entre ponto fixo (x) e inteiro (n). */
#define FP_ADD_INT(x, n) ((x) + (n) * FP_F)
#define FP_SUB_INT(x, n) ((x) - (n) * FP_F)

/* Multiplicacao e divisao entre dois pontos fixos (usa int64_t para evitar overflow). */
#define FP_MUL(x, y) ((fixed_t) (((int64_t) (x) * (y)) / FP_F))
#define FP_DIV(x, y) ((fixed_t) (((int64_t) (x) * FP_F) / (y)))

/* Multiplicacao e divisao entre ponto fixo (x) e inteiro (n). */
#define FP_MUL_INT(x, n) ((x) * (n))
#define FP_DIV_INT(x, n) ((x) / (n))

/* =========================== */

/* Valor aleatorio para o membro `magic' da struct thread.
   Usado para detectar estouro de pilha (stack overflow). Veja o comentario
   no topo de thread.h para detalhes. */
#define THREAD_MAGIC 0xcd6abf4b
#define A 55

/* ========== MLFQS ========== */
/* Compara a prioridade de duas threads na lista. */
static bool priority_less (const struct list_elem *a, const struct list_elem *b, void *aux);
/* =========================== */

/* Lista de processos no estado THREAD_READY, ou seja, processos
   que estao prontos para executar, mas nao estao rodando no momento. */
static struct list ready_list;

/* Lista de todos os processos. Os processos sao adicionados a esta lista
   quando sao escalonados pela primeira vez e removidos quando terminam. */
static struct list all_list;

/* Thread ociosa (idle). */
static struct thread *idle_thread;

/* Thread inicial, a thread que executa init.c:main(). */
static struct thread *initial_thread;

/* Lock utilizado por allocate_tid(). */
static struct lock tid_lock;

/* Quadro de pilha (stack frame) para kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Endereco de retorno. */
    thread_func *function;      /* Funcao a ser chamada. */
    void *aux;                  /* Dados auxiliares para a funcao. */
  };

/* Estatisticas. */
static long long idle_ticks;    /* Ticks do timer gastos em ociosidade. */
static long long kernel_ticks;  /* Ticks do timer gastos em threads do kernel. */
static long long user_ticks;    /* Ticks do timer gastos em programas de usuario. */

/* Escalonamento. */
#define TIME_SLICE 4            /* Quantidade de ticks do timer dados a cada thread. */
static unsigned thread_ticks;   /* Ticks do timer desde o ultimo yield. */

/* Se false (padrao), usa o escalonador round-robin.
   Se true, usa o escalonador de filas de multiplos niveis (MLFQS).
   Controlado pela opcao de linha de comando do kernel "-o mlfqs". */
bool thread_mlfqs;

/* ========== MLFQS ========== */
/* Carga media do sistema em ponto fixo 17.14. */
static fixed_t load_avg;
/* =========================== */

/* ========== MLFQS ========== */

/* Recalcula a prioridade de uma thread usando a formula do MLFQS:
   priority = PRI_MAX - (recent_cpu / 4) - (nice * 2). */
static void
mlfqs_update_priority (struct thread *t)
{
  fixed_t priority_fp;
  int priority;

  /* A thread idle nao participa do calculo de prioridade do MLFQS. */
  if (t == idle_thread)
    return;

  priority_fp = FP_SUB ( FP_SUB_INT (FP_FROM_INT (PRI_MAX), 2 * t->nice), FP_DIV_INT (t->recent_cpu, 4));

  priority = FP_TO_INT_ZERO (priority_fp);

  /* Garante que a prioridade fique dentro dos limites validos [0, 63]. */
  if (priority > PRI_MAX)
    priority = PRI_MAX;

  if (priority < PRI_MIN)
    priority = PRI_MIN;

  t->priority = priority;
}

/* Atualiza a prioridade de uma thread durante a iteracao com thread_foreach. */
static void
mlfqs_update_priority_callback (struct thread *t, void *aux UNUSED)
{
  mlfqs_update_priority (t);
}

/* =========================== */

/* ========== MLFQS ========== */
/* Atualiza a carga media do sistema (load_avg) a cada segundo. */
static void
mlfqs_update_load_avg (void)
{
  int ready_threads = list_size (&ready_list);

  /* Inclui a thread em execucao na contagem, exceto se for a thread idle. */
  if (thread_current () != idle_thread)
    ready_threads++;

  /* Formula: load_avg = (59/60) * load_avg + (1/60) * ready_threads. */
  load_avg = FP_ADD ( FP_MUL ( FP_DIV_INT (FP_FROM_INT (59), 60), load_avg), FP_DIV_INT (FP_FROM_INT (ready_threads), 60));
}

/* Atualiza o uso recente de CPU (recent_cpu) de uma thread a cada segundo. */
static void
mlfqs_update_recent_cpu (struct thread *t, void *aux UNUSED)
{
  fixed_t twice_load;
  fixed_t coefficient;

  /* A thread idle nao participa. */
  if (t == idle_thread)
    return;

  /* Calcula o coeficiente (2 * load_avg) / (2 * load_avg + 1) primeiro para evitar overflow. */
  twice_load = FP_MUL_INT (load_avg, 2);

  coefficient = FP_DIV (twice_load, FP_ADD_INT (twice_load, 1));

  /* Formula: recent_cpu = coefficient * recent_cpu + nice. */
  t->recent_cpu = FP_ADD_INT (FP_MUL (coefficient, t->recent_cpu), t->nice);
}
/* =========================== */

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Inicializa o sistema de threads transformando o codigo atualmente em execucao
   em uma thread. Isso nao funcionaria em geral, sendo possivel neste caso apenas
   porque loader.S teve o cuidado de colocar a base da pilha no limite de uma pagina.

   Tambem inicializa a fila de prontos e o lock de tid.

   Apos chamar esta funcao, certifique-se de inicializar o alocador de paginas
   antes de tentar criar qualquer thread com thread_create().

   Nao e seguro chamar thread_current() ate que esta funcao termine. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Configura a estrutura de thread para a thread em execucao inicial. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);

  /* ========== MLFQS ========== */
  /* Calcula a prioridade inicial caso o MLFQS esteja ativo. */
  if (thread_mlfqs)
    mlfqs_update_priority (initial_thread);
  /* =========================== */

  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Inicia o escalonamento preemptivo de threads habilitando as interrupcoes.
   Tambem cria a thread idle. */
void
thread_start (void) 
{
  /* Cria a thread idle. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Inicia o escalonamento preemptivo de threads. */
  intr_enable ();

  /* Aguarda a thread idle inicializar a variavel idle_thread. */
  sema_down (&idle_started);
}

/* Chamada pelo tratador de interrupcoes do timer a cada tick do relogio.
   Portanto, esta funcao executa em um contexto de interrupcao externa. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Atualiza as estatisticas. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* ========== MLFQS ========== */
  if (thread_mlfqs) {
    int64_t current_ticks = timer_ticks ();

    /* Incrementa o recent_cpu a cada tick, exceto para a thread idle. */
    if (t != idle_thread)
      t->recent_cpu = FP_ADD_INT (t->recent_cpu, 1);

    /* Atualiza o load_avg e o recent_cpu de todas as threads a cada segundo. */
    if (current_ticks % TIMER_FREQ == 0) {
        mlfqs_update_load_avg ();
        thread_foreach (mlfqs_update_recent_cpu, NULL);
      }

    /* Recalcula a prioridade de todas as threads a cada 4 ticks. */
    if (current_ticks % 4 == 0) {
        thread_foreach (mlfqs_update_priority_callback, NULL);
      }
  }

  /* Faz preempcao se houver uma thread pronta com prioridade maior que a atual. */
  if (thread_mlfqs && !list_empty (&ready_list)) {
    struct list_elem *max_elem = list_max (&ready_list, priority_less, NULL);
    struct thread *highest_ready = list_entry (max_elem, struct thread, elem);
    if (highest_ready->priority > t->priority)
    intr_yield_on_return ();
  }
  /* =========================== */

  /* Aplica preempcao ao atingir a fatia de tempo (TIME_SLICE). */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Imprime as estatisticas das threads. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Cria uma nova thread de kernel chamada NAME com a PRIORITY inicial dada,
   que executa FUNCTION passando AUX como argumento, e a adiciona na fila
   de prontos. Retorna o identificador da nova thread, ou TID_ERROR se a
   criacao falhar.

   Se thread_start() ja foi chamada, entao a nova thread pode ser escalonada
   antes de thread_create() retornar. Ela pode ate mesmo encerrar antes de
   thread_create() retornar. Caso contrario, a thread original pode rodar
   por qualquer tempo antes que a nova thread seja escalonada. Use um
   semaforo ou outra forma de sincronizacao se precisar garantir a ordem.

   O codigo fornecido define o membro `priority' da nova thread como PRIORITY,
   mas no modo MLFQS a prioridade real e recalculada automaticamente. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  int new_thread_priority; /* Guarda a prioridade da nova thread no MLFQS. */
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Aloca memoria para a thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Inicializa a thread. */
  init_thread (t, name, priority);

  /* ========== MLFQS ========== */
  /* A nova thread herda o nice e o recent_cpu da thread pai e atualiza sua prioridade. */
  if (thread_mlfqs && function != idle) {
    enum intr_level old_level = intr_disable ();
    struct thread *parent = thread_current ();

    t->nice = parent->nice;
    t->recent_cpu = parent->recent_cpu;

    mlfqs_update_priority (t);

    intr_set_level (old_level);
  }
  /* =========================== */

  tid = t->tid = allocate_tid ();

  /* Quadro de pilha para kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Quadro de pilha para switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Quadro de pilha para switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* ========== MLFQS ========== */
  /* Adiciona na fila de prontos. */
  new_thread_priority = t->priority;
  thread_unblock (t);

  /* Cede a CPU se a nova thread tiver prioridade maior que a thread atual. */
  if (thread_mlfqs && !intr_context () && intr_get_level () == INTR_ON && new_thread_priority > thread_current ()->priority) {
     thread_yield ();
    }
  /* =========================== */

  return tid;
}

/* Coloca a thread atual para dormir. Ela nao sera escalonada novamente
   ate ser acordada por thread_unblock().

   Esta funcao deve ser chamada com as interrupcoes desligadas. Geralmente
   e uma ideia melhor usar uma das primitivas de sincronizacao em synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Muda uma thread bloqueada T para o estado pronto para rodar.
   Isto e um erro se T nao estiver bloqueada. (Use thread_yield() para
   tornar a thread em execucao pronta.)

   Esta funcao nao faz preempcao da thread em execucao. Isso pode ser
   importante: se o chamador desabilitou as interrupcoes por conta propria,
   ele pode esperar desbloquear uma thread atomicamente e atualizar outros dados. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Retorna o nome da thread em execucao. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Retorna a thread em execucao.
   Isto e running_thread() mais algumas verificacoes de seguranca.
   Veja o grande comentario no topo de thread.h para detalhes. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Garante que T e realmente uma thread.
     Se qualquer uma dessas assercoes disparar, sua thread pode ter
     estourado a pilha. Cada thread tem menos de 4 kB de pilha, entao
     alguns vetores grandes ou recursao moderada podem causar estouro de pilha. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Retorna o tid da thread em execucao. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Desescalona a thread atual e a destroi. Nunca retorna ao chamador. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove a thread da lista de todas as threads, define seu status como
     dying e escalona outro processo. Esse processo nos destruira quando
     chamar thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Cede a CPU. A thread atual nao e colocada para dormir e pode ser
   escalonada novamente em seguida a criterio do escalonador. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoca a funcao 'func' em todas as threads, passando 'aux'.
   Esta funcao deve ser chamada com as interrupcoes desligadas. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Define a prioridade da thread atual como NEW_PRIORITY (ignorado no MLFQS). */
void
thread_set_priority (int new_priority) 
{
  if (!thread_mlfqs)
    thread_current ()->priority = new_priority;
}

/* Retorna a prioridade da thread atual. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Define o valor de nice da thread atual como NICE e recalcula sua prioridade. */
void
thread_set_nice (int nice)
{
  /* ========== MLFQS ========== */
  enum intr_level old_level;
  struct thread *cur;
  struct list_elem *e;
  bool should_yield = false;

  ASSERT (nice >= -20 && nice <= 20);

  old_level = intr_disable ();
  cur = thread_current ();

  cur->nice = nice;

  if (thread_mlfqs) {
      mlfqs_update_priority (cur);

      /* Verifica se existe thread pronta com maior prioridade. */
      for (e = list_begin (&ready_list); e != list_end (&ready_list); e = list_next (e)) {
          struct thread *ready = list_entry (e, struct thread, elem);

          if (ready->priority > cur->priority) {
              should_yield = true;
              break;
            }
        }
    }

  intr_set_level (old_level);

  /* Cede a CPU caso a nova prioridade tenha ficado menor que a de outra thread pronta. */
  if (should_yield)
    thread_yield ();
  /* =========================== */
}

/* Retorna o valor de nice da thread atual. */
int
thread_get_nice (void) 
{
  /* ========== MLFQS ========== */
  return thread_current ()->nice;
  /* =========================== */
}

/* Retorna 100 vezes a carga media do sistema (load_avg) arredondada. */
int
thread_get_load_avg (void) 
{
  /* ========== MLFQS ========== */
  enum intr_level old_level;
  int result;

  old_level = intr_disable ();

  result = FP_TO_INT_NEAREST ((int64_t) load_avg * 100);

  intr_set_level (old_level);

  return result;
  /* =========================== */
}

/* Retorna 100 vezes o valor de recent_cpu da thread atual arredondado. */
int
thread_get_recent_cpu (void) 
{
  /* ========== MLFQS ========== */
  enum intr_level old_level;
  int result;

  old_level = intr_disable ();

  result = FP_TO_INT_NEAREST ((int64_t) thread_current ()->recent_cpu * 100);

  intr_set_level (old_level);

  return result;
  /* =========================== */
}

/* Thread ociosa (idle). Executa quando nenhuma outra thread esta pronta para rodar.

   A thread idle e colocada inicialmente na ready_list por thread_start().
   Ela sera escalonada uma vez no inicio, momento em que inicializa idle_thread,
   libera ("up") o semaforo passado para permitir que thread_start() continue,
   e bloqueia imediatamente. Depois disso, a thread idle nunca mais aparece na
   ready_list. Ela e retornada por next_thread_to_run() como um caso especial
   quando a ready_list esta vazia. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Deixa outra thread executar. */
      intr_disable ();
      thread_block ();

      /* Reabilita as interrupcoes e aguarda pela proxima.

         A instrucao `sti' desabilita interrupcoes ate a conclusao da proxima
         instrucao, de modo que estas duas instrucoes sao executadas atomicamente.
         Essa atomicidade e importante; caso contrario, uma interrupcao poderia
         ser tratada entre reabilitar as interrupcoes e aguardar a proxima
         ocorrer, desperdicando ate um tick inteiro de relogio.

         Veja [IA32-v2a] "HLT", [IA32-v2b] "STI" e [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Funcao usada como base para uma thread de kernel. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* O escalonador executa com as interrupcoes desligadas. */
  function (aux);       /* Executa a funcao da thread. */
  thread_exit ();       /* Se function() retornar, encerra a thread. */
}

/* Retorna a thread em execucao. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copia o ponteiro de pilha da CPU para `esp' e arredonda para baixo ate o
     inicio da pagina. Como `struct thread' fica sempre no inicio de uma pagina
     e o ponteiro de pilha fica em algum lugar no meio, isso localiza a thread atual. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Retorna true se T parece apontar para uma thread valida. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Faz a inicializacao basica de T como uma thread bloqueada chamada NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;

  /* ========== MLFQS ========== */
  /* Inicializa nice e recent_cpu zerados. */
  t->nice = 0;
  t->recent_cpu = FP_FROM_INT (0);
  /* =========================== */

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Aloca um quadro de SIZE bytes no topo da pilha da thread T e
   retorna um ponteiro para a base do quadro. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Os dados da pilha sao sempre alocados em unidades de tamanho de palavra (word-size). */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* ========== MLFQS ========== */
/* Compara duas threads pela prioridade (retorna true se 'ta' tem prioridade menor que 'tb'). */
static bool
priority_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);

  const struct thread *tb = list_entry (b, struct thread, elem);

  return ta->priority < tb->priority;
}
/* =========================== */

/* Escolhe e retorna a proxima thread a ser escalonada. Deve retornar uma
   thread da fila de prontos, a menos que a fila esteja vazia. (Se a thread
   em execucao puder continuar rodando, entao ela estara na fila de prontos.)
   Se a fila de prontos estiver vazia, retorna idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  /* ========== MLFQS ========== */
  if (thread_mlfqs) {
      struct list_elem *e;

      /* Seleciona e remove da ready_list a thread pronta com a maior prioridade. */
      e = list_max (&ready_list, priority_less, NULL);

      list_remove (e);

      return list_entry (e, struct thread, elem);
    }

  /* Preserva o escalonador original quando o MLFQS esta desativado. */
  return list_entry (list_pop_front (&ready_list), struct thread, elem);
  /* =========================== */
}

/* Completa uma troca de threads ativando as tabelas de paginas da nova
   thread e, se a thread anterior estiver morrendo, destruindo-a.

   Na invocacao desta funcao, acabamos de trocar da thread PREV, a nova
   thread ja esta rodando e as interrupcoes ainda estao desabilitadas.
   Esta funcao e normalmente invocada por schedule() como sua ultima
   acao antes de retornar, mas na primeira vez que uma thread e escalonada
   ela e chamada por switch_entry() (veja switch.S).

   Nao e seguro chamar printf() ate que a troca de threads termine.
   Na pratica, isso significa que printf()s devem ser adicionados no
   final da funcao.

   Apos esta funcao e seu chamador retornarem, a troca de threads esta completa. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Marca a thread atual como em execucao. */
  cur->status = THREAD_RUNNING;

  /* Inicia uma nova fatia de tempo (time slice). */
  thread_ticks = 0;

#ifdef USERPROG
  /* Ativa o novo espaco de enderecamento. */
  process_activate ();
#endif

  /* Se a thread da qual saimos esta morrendo, destroi sua struct thread.
     Isso deve acontecer tarde para que thread_exit() nao destrua sua
     propria pilha prematuramente. (Nao liberamos initial_thread porque
     sua memoria nao foi obtida via palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Escalona um novo processo. Na entrada, as interrupcoes devem estar
   desligadas e o estado do processo em execucao deve ter sido alterado
   de running para outro estado. Esta funcao encontra outra thread para
   rodar e troca para ela.

   Nao e seguro chamar printf() ate que thread_schedule_tail() termine. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  /* TODO:
   * Ver de usar o thread_block, mas para o schedule 
   * tem de verificar se uma thread esta bloqueada, alem de implementar 
   * o unblock com o tempo
   * */
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Retorna um tid para ser usado por uma nova thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Deslocamento (offset) do membro `stack' dentro de `struct thread'.
   Usado por switch.S, que nao consegue descobrir isso sozinho. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);