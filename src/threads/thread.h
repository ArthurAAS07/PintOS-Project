#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>

/* Estados no ciclo de vida de uma thread. */
enum thread_status
  {
    THREAD_RUNNING,     /* Thread em execucao. */
    THREAD_READY,       /* Nao esta rodando, mas pronta para executar. */
    THREAD_BLOCKED,     /* Aguardando um evento disparar. */
    THREAD_DYING        /* Prestes a ser destruida. */
  };

/* Tipo identificador de thread.
   Voce pode redefinir este tipo para o que preferir. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Valor de erro para tid_t. */

/* Prioridades das threads. */
#define PRI_MIN 0                       /* Prioridade mais baixa. */
#define PRI_DEFAULT 31                  /* Prioridade padrao. */
#define PRI_MAX 63                      /* Prioridade mais alta. */

/* Uma thread de kernel ou processo de usuario.

   Cada estrutura de thread e armazenada em sua propria pagina de 4 kB.
   A estrutura da thread em si fica na base da pagina (no deslocamento 0).
   O restante da pagina e reservado para a pilha de kernel da thread,
   que cresce para baixo a partir do topo da pagina (em 4 kB).
   Veja a ilustracao:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   A consequencia disso tem dois aspectos:

      1. Primeiro, `struct thread' nao pode crescer demais. Se crescer,
         nao havera espaco suficiente para a pilha do kernel. Nossa
         `struct thread' base tem apenas alguns bytes de tamanho.
         Ela deve permanecer bem abaixo de 1 kB.

      2. Segundo, as pilhas do kernel nao podem crescer demais. Se uma
         pilha estourar, ela corrompera o estado da thread. Assim,
         funcoes do kernel nao devem alocar grandes estruturas ou
         vetores como variaveis locais nao-estaticas. Use alocacao
         dinamica com malloc() ou palloc_get_page() em vez disso.

   O primeiro sintoma de qualquer um desses problemas provavelmente sera
   uma falha de assercao em thread_current(), que verifica se o membro
   `magic' da `struct thread' em execucao esta definido como THREAD_MAGIC.
   O estouro de pilha normalmente altera esse valor, disparando a assercao. */
/* O membro `elem' tem finalidade dupla. Ele pode ser um elemento na
   fila de prontos (thread.c), ou um elemento em uma lista de espera de
   semaforo (synch.c). Ele so pode ser usado dessas duas maneiras porque
   sao mutuamente exclusivas: apenas uma thread no estado pronto fica na
   fila de prontos, enquanto apenas uma thread no estado bloqueado fica
   em uma lista de espera de semaforo. */
struct thread
  {
    /* Gerenciado por thread.c. */
    tid_t tid;                          /* Identificador da thread. */
    enum thread_status status;          /* Estado da thread. */
    char name[16];                      /* Nome (para fins de depuracao). */
    uint8_t *stack;                     /* Ponteiro de pilha salvo. */
    int priority;                       /* Prioridade. */
    struct list_elem allelem;           /* Elemento para a lista de todas as threads. */

    /* Compartilhado entre thread.c e synch.c. */
    struct list_elem elem;              /* Elemento de lista. */

    /* ==========Alarm Clock========== */
    /* Usados apenas enquanto a thread esta dormindo na sleeping_list. */
    int64_t wake_tick;
    struct list_elem sleep_elem;
    /* =============================== */

    /* ========== MLFQS ========== */
    /* Valor nice da thread (-20 a 20). */
    int nice;
    /* Uso recente de CPU, representado em ponto fixo 17.14. */
    int32_t recent_cpu;
    /* =========================== */

#ifdef USERPROG
    /* Gerenciado por userprog/process.c. */
    uint32_t *pagedir;                  /* Diretorio de paginas. */
#endif

    /* Gerenciado por thread.c. */
    unsigned magic;                     /* Detecta estouro de pilha. */
  };

/* Se false (padrao), usa o escalonador round-robin.
   Se true, usa o escalonador de filas de multiplos niveis (MLFQS).
   Controlado pela opcao de linha de comando do kernel "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Executa alguma operacao na thread t, dado um argumento auxiliar AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

/* Funcoes do escalonador MLFQS para manipular nice, recent_cpu e load_avg. */
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */