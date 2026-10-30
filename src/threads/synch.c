/* Este arquivo e derivado do codigo-fonte do sistema operacional
   instrucional Nachos. O aviso de direitos autorais do Nachos e
   reproduzido integralmente abaixo. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Inicializa o semaforo SEMA com o valor VALUE. Um semaforo e um
   inteiro nao negativo acompanhado de dois operadores atomicos para
   manipula-lo:

   - down ou "P": aguarda o valor se tornar positivo e entao o
     decrementa.

   - up ou "V": incrementa o valor (e acorda uma thread que estiver
     esperando, se houver alguma). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Operacao Down ou "P" em um semaforo. Aguarda o valor de SEMA se
   tornar positivo e entao o decrementa atomicamente.

   Esta funcao pode dormir, portanto nao deve ser chamada dentro de um
   tratador de interrupcao. Esta funcao pode ser chamada com as interrupcoes
   desabilitadas, mas se ela dormir, a proxima thread escalonada provavelmente
   voltara a ligar as interrupcoes. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Operacao Down ou "P" em um semaforo, mas apenas se o semaforo ja
   nao estiver em 0. Retorna true se o semaforo for decrementado, ou
   false caso contrario.

   Esta funcao pode ser chamada a partir de um tratador de interrupcao. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* ========== MLFQS ========== */
/* Compara as prioridades de duas threads em uma lista.
   Serve de apoio para o list_max encontrar quem tem a maior prioridade. */
static bool
sema_priority_less (const struct list_elem *a,
                    const struct list_elem *b,
                    void *aux UNUSED)
{
  const struct thread *ta = list_entry (a, struct thread, elem);

  const struct thread *tb = list_entry (b, struct thread, elem);

  return ta->priority < tb->priority;
}
/* =========================== */

/* Operacao Up ou "V" em um semaforo. Incrementa o valor de SEMA e acorda
   uma thread da lista de espera, priorizando a de maior prioridade no MLFQS. */
void
sema_up (struct semaphore *sema)
{
  enum intr_level old_level;
  bool should_preempt = false;

  ASSERT (sema != NULL);

  old_level = intr_disable ();

  /* Verifica se ha alguma thread esperando pelo semaforo. */
  if (!list_empty (&sema->waiters)) {
      struct list_elem *e;
      struct thread *awakened;

      /* Se o MLFQS estiver ativo, busca e remove a thread de maior prioridade
         da fila de espera; caso contrario, pega a primeira da fila normalmente. */
      if (thread_mlfqs) {
          e = list_max (&sema->waiters, sema_priority_less, NULL);
          list_remove (e);
        }
      else {
        e = list_pop_front (&sema->waiters);
      }
      awakened = list_entry (e, struct thread, elem);

      /* Marca se a thread acordada tem prioridade maior que a atual para ceder a CPU depois. */
      should_preempt = thread_mlfqs && awakened->priority > thread_current ()->priority;

      /* Desbloqueia a thread escolhida, colocando-a na ready_list. */
      thread_unblock (awakened);
    }

  /* Finaliza a liberacao do semaforo antes da preempcao. */
  sema->value++;
  intr_set_level (old_level);

  /* Se a thread recem-acordada tiver prioridade maior, entrega a CPU para ela. */
  if (should_preempt) {
      if (intr_context ())
        intr_yield_on_return ();
      else if (old_level == INTR_ON)
        thread_yield ();
    }
}

static void sema_test_helper (void *sema_);

/* Autoteste para semaforos que faz o controle alternar ("ping-pong")
   entre um par de threads. Insira chamadas a printf() para ver o que
   esta acontecendo. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Funcao de thread usada por sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Inicializa o LOCK. Um lock pode ser mantido por no maximo uma unica
   thread a qualquer momento. Nossos locks nao sao "recursivos", isto e,
   e um erro para a thread que atualmente detem um lock tentar adquirir
   esse mesmo lock novamente.

   Um lock e uma especializacao de um semaforo com valor inicial 1.
   A diferenca entre um lock e um semaforo desse tipo e dupla. Primeiro,
   um semaforo pode ter um valor maior que 1, mas um lock so pode pertencer
   a uma unica thread por vez. Segundo, um semaforo nao tem um dono, o que
   significa que uma thread pode dar "down" no semaforo e outra dar "up",
   mas com um lock a mesma thread deve tanto adquiri-lo quanto libera-lo.
   Quando essas restricoes se mostram excessivas, e um bom sinal de que um
   semaforo deve ser usado em vez de um lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* Adquire o LOCK, dormindo ate que ele fique disponivel se necessario.
   O lock nao deve estar ja em posse da thread atual.

   Esta funcao pode dormir, portanto nao deve ser chamada dentro de um
   tratador de interrupcao. Esta funcao pode ser chamada com interrupcoes
   desabilitadas, mas as interrupcoes serao religadas se precisarmos dormir. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  sema_down (&lock->semaphore);
  lock->holder = thread_current ();
}

/* Tenta adquirir o LOCK e retorna true em caso de sucesso ou false em
   caso de falha. O lock nao deve estar ja em posse da thread atual.

   Esta funcao nao dorme, portanto pode ser chamada dentro de um tratador
   de interrupcao. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Libera o LOCK, que deve pertencer a thread atual.

   Um tratador de interrupcao nao pode adquirir um lock, entao nao faz
   sentido tentar liberar um lock dentro de um tratador de interrupcao. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}

/* Retorna true se a thread atual detem o LOCK, ou false caso contrario.
   (Observe que testar se alguma outra thread detem um lock geraria
   condicao de corrida.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* Um elemento de semaforo dentro de uma lista (usado em variaveis de condicao). */
struct semaphore_elem 
  {
    struct list_elem elem;              /* Elemento de lista. */
    struct semaphore semaphore;         /* Este semaforo. */
  };

/* Inicializa a variavel de condicao COND. Uma variavel de condicao permite
   que um trecho de codigo sinalize uma condicao e que o codigo cooperante
   receba o sinal e aja sobre ele. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Libera atomicamente o LOCK e aguarda COND ser sinalizada por algum outro
   trecho de codigo. Apos COND ser sinalizada, o LOCK e readquirido antes de
   retornar. O LOCK deve estar em posse da thread antes de chamar esta funcao.

   O monitor implementado por esta funcao segue o estilo "Mesa", e nao o
   estilo "Hoare", ou seja, enviar e receber um sinal nao constituem uma
   operacao atomica. Assim, tipicamente o chamador deve verificar novamente
   a condicao apos a espera terminar e, se necessario, aguardar outra vez.

   Uma determinada variavel de condicao esta associada a apenas um unico lock,
   mas um mesmo lock pode estar associado a qualquer numero de variaveis de
   condicao. Isto e, existe um mapeamento de um para muitos entre locks e
   variaveis de condicao.

   Esta funcao pode dormir, portanto nao deve ser chamada dentro de um
   tratador de interrupcao. Esta funcao pode ser chamada com interrupcoes
   desabilitadas, mas as interrupcoes serao religadas se precisarmos dormir. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* Se houver alguma thread esperando em COND (protegida por LOCK), esta
   funcao sinaliza uma delas para acordar de sua espera.
   O LOCK deve estar em posse da thread antes de chamar esta funcao.

   Um tratador de interrupcao nao pode adquirir um lock, portanto nao faz
   sentido tentar sinalizar uma variavel de condicao dentro de um tratador
   de interrupcao. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    sema_up (&list_entry (list_pop_front (&cond->waiters),
                          struct semaphore_elem, elem)->semaphore);
}

/* Acorda todas as threads, se houver alguma, que estejam esperando em COND
   (protegida por LOCK). O LOCK deve estar em posse da thread antes de chamar
   esta funcao.

   Um tratador de interrupcao nao pode adquirir um lock, portanto nao faz
   sentido tentar sinalizar uma variavel de condicao dentro de um tratador
   de interrupcao. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}