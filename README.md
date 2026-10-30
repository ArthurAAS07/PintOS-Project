# PintOS — Alarm Clock e Advanced Scheduler (MLFQS)

Implementação de duas funcionalidades do subsistema de **threads** do [PintOS](https://web.stanford.edu/class/cs140/projects/pintos/pintos.html): **Alarm Clock**, para suspender threads sem espera ocupada, e **Advanced Scheduler (MLFQS)**, baseado no escalonador 4.4BSD, para ajustar prioridades dinamicamente conforme o uso da CPU.

O projeto foi desenvolvido em **C**, sobre a infraestrutura de threads, interrupções e sincronização fornecida pelo PintOS.

## Funcionalidades implementadas

| Funcionalidade | Objetivo | Arquivos envolvidos |
| --- | --- | --- |
| **Alarm Clock** | Substituir a espera ocupada por bloqueio e despertar no tick programado. | [`src/devices/timer.c`](src/devices/timer.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Advanced Scheduler (MLFQS)** | Calcular prioridades dinamicamente usando `nice`, `recent_cpu` e `load_avg`. | [`src/threads/thread.c`](src/threads/thread.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Integração com semáforos** | Acordar a thread de maior prioridade no modo MLFQS e permitir preempção após liberar o semáforo. | [`src/threads/synch.c`](src/threads/synch.c) |

## 1. Alarm Clock

### Problema original

Na implementação inicial, `timer_sleep()` consultava repetidamente o tempo decorrido e chamava `thread_yield()`. Mesmo sem trabalho útil, a thread permanecia elegível ao escalonamento, caracterizando **busy waiting** (espera ocupada).

### Solução adotada

Em [`src/devices/timer.c`](src/devices/timer.c):

- **`sleeping_list`**: lista de threads adormecidas, ordenada pelo tick de despertar.
- **`timer_init()`**: inicializa a lista de threads adormecidas.
- **`wake_before()`**: compara os instantes de despertar e mantém a ordenação.
- **`timer_sleep()`**: calcula `wake_tick`, insere a thread na lista e chama `thread_block()` com interrupções temporariamente desabilitadas para proteger a operação.
- **`timer_interrupt()`**: incrementa o contador de ticks, retira da lista as threads cujo prazo chegou e as torna prontas novamente com `thread_unblock()`.

Em [`src/threads/thread.h`](src/threads/thread.h), foram adicionados à `struct thread`:

```c
int64_t wake_tick;            /* Tick em que a thread deve acordar. */
struct list_elem sleep_elem;  /* Elemento próprio da lista de sono. */
```

O uso de `sleep_elem` evita conflitos com `elem`, que já é compartilhado entre a lista de prontas e as listas de espera de semáforos.

**Fluxo:** `timer_sleep()` → `THREAD_BLOCKED` → interrupção no tick programado → `thread_unblock()` → `THREAD_READY`.

## 2. Advanced Scheduler — MLFQS (4.4BSD)

O **Multi-Level Feedback Queue Scheduler (MLFQS)** prioriza threads segundo o consumo recente de CPU e o valor `nice`. O modo é ativado pelo argumento de inicialização **`-mlfqs`**. Embora o modelo conceitual descreva 64 níveis de prioridade, esta implementação mantém **uma única `ready_list`** e seleciona o elemento de maior prioridade.

### Informações mantidas pelo escalonador

Em [`src/threads/thread.h`](src/threads/thread.h), foram acrescentados à `struct thread`:

```c
int nice;           /* Ajuste de prioridade da thread. */
int32_t recent_cpu; /* Histórico de CPU em ponto fixo 17.14. */
```

Em [`src/threads/thread.c`](src/threads/thread.c), a variável global estática `load_avg` representa a carga média do sistema. Valores fracionários são manipulados com **aritmética de ponto fixo 17.14** (`2^14 = 16384` como fator de escala), evitando o uso de ponto flutuante no kernel. Multiplicações e divisões de valores em ponto fixo utilizam intermediários de 64 bits.

### Fórmulas utilizadas

**Prioridade dinâmica:**

```text
priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
```

O resultado é convertido para inteiro e limitado ao intervalo **0 a 63**.

**Uso recente de CPU:**

```text
recent_cpu = ((2 * load_avg) / (2 * load_avg + 1)) * recent_cpu + nice
```

**Carga média:**

```text
load_avg = (59/60) * load_avg + (1/60) * ready_threads
```

`ready_threads` considera as threads prontas e a thread em execução, exceto a *idle thread*.

### Frequência das atualizações

As atualizações são integradas à função **`thread_tick()`**, em `thread.c`:

| Frequência | Operação |
| --- | --- |
| **A cada tick** | Incrementar `recent_cpu` da thread em execução, exceto a idle. |
| **A cada segundo** (`TIMER_FREQ` ticks) | Recalcular `load_avg` e o `recent_cpu` de todas as threads. |
| **A cada 4 ticks** | Recalcular a prioridade de todas as threads. |

### Funções implementadas ou adaptadas em `thread.c`

| Função | Responsabilidade |
| --- | --- |
| `mlfqs_update_priority()` | Recalcular e limitar a prioridade de uma thread. |
| `mlfqs_update_recent_cpu()` | Aplicar a fórmula de consumo recente de CPU. |
| `mlfqs_update_load_avg()` | Atualizar a carga média do sistema. |
| `mlfqs_update_priority_callback()` | Permitir atualizar prioridades usando `thread_foreach()`. |
| `thread_init()` / `init_thread()` | Inicializar os campos e a prioridade da thread principal. |
| `thread_create()` | Herdar `nice` e `recent_cpu` da thread criadora, calcular a prioridade inicial e verificar preempção. |
| `thread_tick()` | Executar atualizações periódicas e solicitar preempção quando necessário. |
| `priority_less()` / `next_thread_to_run()` | Escolher a thread pronta de maior prioridade usando `list_max()`. |
| `thread_set_nice()` / `thread_get_nice()` | Alterar e consultar o valor `nice`; recalcular a prioridade quando necessário. |
| `thread_get_recent_cpu()` / `thread_get_load_avg()` | Retornar as respectivas métricas multiplicadas por 100 e arredondadas. |
| `thread_set_priority()` | Ignorar a definição manual de prioridade quando MLFQS estiver habilitado. |

A *idle thread* não participa dos cálculos de `recent_cpu` nem de `load_avg` como thread executável. Em caso de empate de prioridade na lista de prontas, a seleção preserva a ordem entre os elementos empatados.

## 3. Sincronização e preempção

Foi necessária uma alteração localizada em [`src/threads/synch.c`](src/threads/synch.c) para integrar o MLFQS aos semáforos, especialmente no cenário testado por `mlfqs-block`.

- **`sema_priority_less()`**: compara a prioridade das threads que aguardam um semáforo.
- **`sema_up()`**: no modo MLFQS, seleciona a thread de maior prioridade entre as que aguardam; torna-a pronta; **incrementa `sema->value` antes de solicitar a preempção**; e, quando apropriado, usa `thread_yield()` fora de interrupções ou `intr_yield_on_return()` em contexto de interrupção.

A ordem é importante: a operação do semáforo deve terminar **antes** de a thread despertada voltar a executar. Quando MLFQS está desativado, a seleção em `sema_up()` mantém o comportamento original da fila.

> **Escopo:** o projeto implementa o Alarm Clock e o MLFQS. Não implementa integralmente o escalonamento por prioridade com *priority donation* do projeto original do PintOS.

## 4. Compilação e execução dos testes

É necessário ter o ambiente de compilação do PintOS configurado, incluindo as ferramentas utilizadas pelo projeto para executar o kernel no QEMU.

Na raiz do repositório:

```bash
make -C src/threads
cd src/threads/build
```

### Testes do Alarm Clock

```bash
make tests/threads/alarm-single.result \
     tests/threads/alarm-multiple.result \
     tests/threads/alarm-simultaneous.result \
     tests/threads/alarm-zero.result \
     tests/threads/alarm-negative.result
```

### Testes do Advanced Scheduler

```bash
make tests/threads/mlfqs-load-1.result \
     tests/threads/mlfqs-load-60.result \
     tests/threads/mlfqs-load-avg.result \
     tests/threads/mlfqs-recent-1.result \
     tests/threads/mlfqs-fair-2.result \
     tests/threads/mlfqs-fair-20.result \
     tests/threads/mlfqs-nice-2.result \
     tests/threads/mlfqs-nice-10.result \
     tests/threads/mlfqs-block.result
```

Para consultar os resultados:

```bash
grep -H . tests/threads/mlfqs-*.result \
  tests/threads/alarm-{single,multiple,simultaneous,zero,negative}.result
```

**Observação:** os alvos `.result` podem reutilizar resultados anteriores se já estiverem atualizados. Para executar novamente os testes, remova os `.result` correspondentes antes de chamar `make`.

### Resultados registrados

No snapshot do projeto utilizado para esta documentação, os arquivos de resultado dos testes selecionados apresentam:

| Componente | Testes aprovados |
| --- | ---: |
| Alarm Clock | **5/5** |
| Advanced Scheduler (MLFQS) | **9/9** |
| **Total** | **14/14** |

Esses resultados correspondem aos testes selecionados presentes no repositório; não representam validação dos testes de *priority donation*, de todos os testes de `make check` ou de testes ocultos.

## 5. Referências

- [Documentação do PintOS — Project 1: Threads](https://web.stanford.edu/class/cs140/projects/pintos/pintos_2.html)
- *Pintos*, Ben Pfaff — especialmente o Apêndice B, **4.4BSD Scheduler**.
- *CSCI 350: Pintos Guide*, Stephen Tsung-Han Sher — seção **Project 1: Threads**.
- [Vídeo de referência — Pintos Project 1 (parte 1)](https://youtu.be/myO2bs5LMak)
- [Vídeo de referência — Pintos Project 1 (parte 2)](https://youtu.be/4-OjMqyygss)
# PintOS-Project
# PintOS — Alarm Clock e Advanced Scheduler (MLFQS)

Implementação de duas funcionalidades do subsistema de **threads** do [PintOS](https://web.stanford.edu/class/cs140/projects/pintos/pintos.html): **Alarm Clock**, para suspender threads sem espera ocupada, e **Advanced Scheduler (MLFQS)**, baseado no escalonador 4.4BSD, para ajustar prioridades dinamicamente conforme o uso da CPU.

O projeto foi desenvolvido em **C**, sobre a infraestrutura de threads, interrupções e sincronização fornecida pelo PintOS.

## Funcionalidades implementadas

| Funcionalidade | Objetivo | Arquivos envolvidos |
| --- | --- | --- |
| **Alarm Clock** | Substituir a espera ocupada por bloqueio e despertar no tick programado. | [`src/devices/timer.c`](src/devices/timer.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Advanced Scheduler (MLFQS)** | Calcular prioridades dinamicamente usando `nice`, `recent_cpu` e `load_avg`. | [`src/threads/thread.c`](src/threads/thread.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Integração com semáforos** | Acordar a thread de maior prioridade no modo MLFQS e permitir preempção após liberar o semáforo. | [`src/threads/synch.c`](src/threads/synch.c) |

## 1. Alarm Clock

### Problema original

Na implementação inicial, `timer_sleep()` consultava repetidamente o tempo decorrido e chamava `thread_yield()`. Mesmo sem trabalho útil, a thread permanecia elegível ao escalonamento, caracterizando **busy waiting** (espera ocupada).

### Solução adotada

Em [`src/devices/timer.c`](src/devices/timer.c):

- **`sleeping_list`**: lista de threads adormecidas, ordenada pelo tick de despertar.
- **`timer_init()`**: inicializa a lista de threads adormecidas.
- **`wake_before()`**: compara os instantes de despertar e mantém a ordenação.
- **`timer_sleep()`**: calcula `wake_tick`, insere a thread na lista e chama `thread_block()` com interrupções temporariamente desabilitadas para proteger a operação.
- **`timer_interrupt()`**: incrementa o contador de ticks, retira da lista as threads cujo prazo chegou e as torna prontas novamente com `thread_unblock()`.

Em [`src/threads/thread.h`](src/threads/thread.h), foram adicionados à `struct thread`:

```c
int64_t wake_tick;            /* Tick em que a thread deve acordar. */
struct list_elem sleep_elem;  /* Elemento próprio da lista de sono. */
```

O uso de `sleep_elem` evita conflitos com `elem`, que já é compartilhado entre a lista de prontas e as listas de espera de semáforos.

**Fluxo:** `timer_sleep()` → `THREAD_BLOCKED` → interrupção no tick programado → `thread_unblock()` → `THREAD_READY`.

## 2. Advanced Scheduler — MLFQS (4.4BSD)

O **Multi-Level Feedback Queue Scheduler (MLFQS)** prioriza threads segundo o consumo recente de CPU e o valor `nice`. O modo é ativado pelo argumento de inicialização **`-mlfqs`**. Embora o modelo conceitual descreva 64 níveis de prioridade, esta implementação mantém **uma única `ready_list`** e seleciona o elemento de maior prioridade.

### Informações mantidas pelo escalonador

Em [`src/threads/thread.h`](src/threads/thread.h), foram acrescentados à `struct thread`:

```c
int nice;           /* Ajuste de prioridade da thread. */
int32_t recent_cpu; /* Histórico de CPU em ponto fixo 17.14. */
```

Em [`src/threads/thread.c`](src/threads/thread.c), a variável global estática `load_avg` representa a carga média do sistema. Valores fracionários são manipulados com **aritmética de ponto fixo 17.14** (`2^14 = 16384` como fator de escala), evitando o uso de ponto flutuante no kernel. Multiplicações e divisões de valores em ponto fixo utilizam intermediários de 64 bits.

### Fórmulas utilizadas

**Prioridade dinâmica:**

```text
priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
```

O resultado é convertido para inteiro e limitado ao intervalo **0 a 63**.

**Uso recente de CPU:**

```text
recent_cpu = ((2 * load_avg) / (2 * load_avg + 1)) * recent_cpu + nice
```

**Carga média:**

```text
load_avg = (59/60) * load_avg + (1/60) * ready_threads
```

`ready_threads` considera as threads prontas e a thread em execução, exceto a *idle thread*.

### Frequência das atualizações

As atualizações são integradas à função **`thread_tick()`**, em `thread.c`:

| Frequência | Operação |
| --- | --- |
| **A cada tick** | Incrementar `recent_cpu` da thread em execução, exceto a idle. |
| **A cada segundo** (`TIMER_FREQ` ticks) | Recalcular `load_avg` e o `recent_cpu` de todas as threads. |
| **A cada 4 ticks** | Recalcular a prioridade de todas as threads. |

### Funções implementadas ou adaptadas em `thread.c`

| Função | Responsabilidade |
| --- | --- |
| `mlfqs_update_priority()` | Recalcular e limitar a prioridade de uma thread. |
| `mlfqs_update_recent_cpu()` | Aplicar a fórmula de consumo recente de CPU. |
| `mlfqs_update_load_avg()` | Atualizar a carga média do sistema. |
| `mlfqs_update_priority_callback()` | Permitir atualizar prioridades usando `thread_foreach()`. |
| `thread_init()` / `init_thread()` | Inicializar os campos e a prioridade da thread principal. |
| `thread_create()` | Herdar `nice` e `recent_cpu` da thread criadora, calcular a prioridade inicial e verificar preempção. |
| `thread_tick()` | Executar atualizações periódicas e solicitar preempção quando necessário. |
| `priority_less()` / `next_thread_to_run()` | Escolher a thread pronta de maior prioridade usando `list_max()`. |
| `thread_set_nice()` / `thread_get_nice()` | Alterar e consultar o valor `nice`; recalcular a prioridade quando necessário. |
| `thread_get_recent_cpu()` / `thread_get_load_avg()` | Retornar as respectivas métricas multiplicadas por 100 e arredondadas. |
| `thread_set_priority()` | Ignorar a definição manual de prioridade quando MLFQS estiver habilitado. |

A *idle thread* não participa dos cálculos de `recent_cpu` nem de `load_avg` como thread executável. Em caso de empate de prioridade na lista de prontas, a seleção preserva a ordem entre os elementos empatados.

## 3. Sincronização e preempção

Foi necessária uma alteração localizada em [`src/threads/synch.c`](src/threads/synch.c) para integrar o MLFQS aos semáforos, especialmente no cenário testado por `mlfqs-block`.

- **`sema_priority_less()`**: compara a prioridade das threads que aguardam um semáforo.
- **`sema_up()`**: no modo MLFQS, seleciona a thread de maior prioridade entre as que aguardam; torna-a pronta; **incrementa `sema->value` antes de solicitar a preempção**; e, quando apropriado, usa `thread_yield()` fora de interrupções ou `intr_yield_on_return()` em contexto de interrupção.

A ordem é importante: a operação do semáforo deve terminar **antes** de a thread despertada voltar a executar. Quando MLFQS está desativado, a seleção em `sema_up()` mantém o comportamento original da fila.

> **Escopo:** o projeto implementa o Alarm Clock e o MLFQS. Não implementa integralmente o escalonamento por prioridade com *priority donation* do projeto original do PintOS.

## 4. Compilação e execução dos testes

É necessário ter o ambiente de compilação do PintOS configurado, incluindo as ferramentas utilizadas pelo projeto para executar o kernel no QEMU.

Na raiz do repositório:

```bash
make -C src/threads
cd src/threads/build
```

### Testes do Alarm Clock

```bash
make tests/threads/alarm-single.result \
     tests/threads/alarm-multiple.result \
     tests/threads/alarm-simultaneous.result \
     tests/threads/alarm-zero.result \
     tests/threads/alarm-negative.result
```

### Testes do Advanced Scheduler

```bash
make tests/threads/mlfqs-load-1.result \
     tests/threads/mlfqs-load-60.result \
     tests/threads/mlfqs-load-avg.result \
     tests/threads/mlfqs-recent-1.result \
     tests/threads/mlfqs-fair-2.result \
     tests/threads/mlfqs-fair-20.result \
     tests/threads/mlfqs-nice-2.result \
     tests/threads/mlfqs-nice-10.result \
     tests/threads/mlfqs-block.result
```

Para consultar os resultados:

```bash
grep -H . tests/threads/mlfqs-*.result \
  tests/threads/alarm-{single,multiple,simultaneous,zero,negative}.result
```

**Observação:** os alvos `.result` podem reutilizar resultados anteriores se já estiverem atualizados. Para executar novamente os testes, remova os `.result` correspondentes antes de chamar `make`.

### Resultados registrados

No snapshot do projeto utilizado para esta documentação, os arquivos de resultado dos testes selecionados apresentam:

| Componente | Testes aprovados |
| --- | ---: |
| Alarm Clock | **5/5** |
| Advanced Scheduler (MLFQS) | **9/9** |
| **Total** | **14/14** |

Esses resultados correspondem aos testes selecionados presentes no repositório; não representam validação dos testes de *priority donation*, de todos os testes de `make check` ou de testes ocultos.

## 5. Referências

- [Documentação do PintOS — Project 1: Threads](https://web.stanford.edu/class/cs140/projects/pintos/pintos_2.html)
- *Pintos*, Ben Pfaff — especialmente o Apêndice B, **4.4BSD Scheduler**.
- *CSCI 350: Pintos Guide*, Stephen Tsung-Han Sher — seção **Project 1: Threads**.
- [Vídeo de referência — Pintos Project 1 (parte 1)](https://youtu.be/myO2bs5LMak)
- [Vídeo de referência — Pintos Project 1 (parte 2)](https://youtu.be/4-OjMqyygss)
# PintOS — Alarm Clock e Advanced Scheduler (MLFQS)

Implementação de duas funcionalidades do subsistema de **threads** do [PintOS](https://web.stanford.edu/class/cs140/projects/pintos/pintos.html): **Alarm Clock**, para suspender threads sem espera ocupada, e **Advanced Scheduler (MLFQS)**, baseado no escalonador 4.4BSD, para ajustar prioridades dinamicamente conforme o uso da CPU.

O projeto foi desenvolvido em **C**, sobre a infraestrutura de threads, interrupções e sincronização fornecida pelo PintOS.

## Funcionalidades implementadas

| Funcionalidade | Objetivo | Arquivos envolvidos |
| --- | --- | --- |
| **Alarm Clock** | Substituir a espera ocupada por bloqueio e despertar no tick programado. | [`src/devices/timer.c`](src/devices/timer.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Advanced Scheduler (MLFQS)** | Calcular prioridades dinamicamente usando `nice`, `recent_cpu` e `load_avg`. | [`src/threads/thread.c`](src/threads/thread.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Integração com semáforos** | Acordar a thread de maior prioridade no modo MLFQS e permitir preempção após liberar o semáforo. | [`src/threads/synch.c`](src/threads/synch.c) |

## 1. Alarm Clock

### Problema original

Na implementação inicial, `timer_sleep()` consultava repetidamente o tempo decorrido e chamava `thread_yield()`. Mesmo sem trabalho útil, a thread permanecia elegível ao escalonamento, caracterizando **busy waiting** (espera ocupada).

### Solução adotada

Em [`src/devices/timer.c`](src/devices/timer.c):

- **`sleeping_list`**: lista de threads adormecidas, ordenada pelo tick de despertar.
- **`timer_init()`**: inicializa a lista de threads adormecidas.
- **`wake_before()`**: compara os instantes de despertar e mantém a ordenação.
- **`timer_sleep()`**: calcula `wake_tick`, insere a thread na lista e chama `thread_block()` com interrupções temporariamente desabilitadas para proteger a operação.
- **`timer_interrupt()`**: incrementa o contador de ticks, retira da lista as threads cujo prazo chegou e as torna prontas novamente com `thread_unblock()`.

Em [`src/threads/thread.h`](src/threads/thread.h), foram adicionados à `struct thread`:

```c
int64_t wake_tick;            /* Tick em que a thread deve acordar. */
struct list_elem sleep_elem;  /* Elemento próprio da lista de sono. */
```

O uso de `sleep_elem` evita conflitos com `elem`, que já é compartilhado entre a lista de prontas e as listas de espera de semáforos.

**Fluxo:** `timer_sleep()` → `THREAD_BLOCKED` → interrupção no tick programado → `thread_unblock()` → `THREAD_READY`.

## 2. Advanced Scheduler — MLFQS (4.4BSD)

O **Multi-Level Feedback Queue Scheduler (MLFQS)** prioriza threads segundo o consumo recente de CPU e o valor `nice`. O modo é ativado pelo argumento de inicialização **`-mlfqs`**. Embora o modelo conceitual descreva 64 níveis de prioridade, esta implementação mantém **uma única `ready_list`** e seleciona o elemento de maior prioridade.

### Informações mantidas pelo escalonador

Em [`src/threads/thread.h`](src/threads/thread.h), foram acrescentados à `struct thread`:

```c
int nice;           /* Ajuste de prioridade da thread. */
int32_t recent_cpu; /* Histórico de CPU em ponto fixo 17.14. */
```

Em [`src/threads/thread.c`](src/threads/thread.c), a variável global estática `load_avg` representa a carga média do sistema. Valores fracionários são manipulados com **aritmética de ponto fixo 17.14** (`2^14 = 16384` como fator de escala), evitando o uso de ponto flutuante no kernel. Multiplicações e divisões de valores em ponto fixo utilizam intermediários de 64 bits.

### Fórmulas utilizadas

**Prioridade dinâmica:**

```text
priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
```

O resultado é convertido para inteiro e limitado ao intervalo **0 a 63**.

**Uso recente de CPU:**

```text
recent_cpu = ((2 * load_avg) / (2 * load_avg + 1)) * recent_cpu + nice
```

**Carga média:**

```text
load_avg = (59/60) * load_avg + (1/60) * ready_threads
```

`ready_threads` considera as threads prontas e a thread em execução, exceto a *idle thread*.

### Frequência das atualizações

As atualizações são integradas à função **`thread_tick()`**, em `thread.c`:

| Frequência | Operação |
| --- | --- |
| **A cada tick** | Incrementar `recent_cpu` da thread em execução, exceto a idle. |
| **A cada segundo** (`TIMER_FREQ` ticks) | Recalcular `load_avg` e o `recent_cpu` de todas as threads. |
| **A cada 4 ticks** | Recalcular a prioridade de todas as threads. |

### Funções implementadas ou adaptadas em `thread.c`

| Função | Responsabilidade |
| --- | --- |
| `mlfqs_update_priority()` | Recalcular e limitar a prioridade de uma thread. |
| `mlfqs_update_recent_cpu()` | Aplicar a fórmula de consumo recente de CPU. |
| `mlfqs_update_load_avg()` | Atualizar a carga média do sistema. |
| `mlfqs_update_priority_callback()` | Permitir atualizar prioridades usando `thread_foreach()`. |
| `thread_init()` / `init_thread()` | Inicializar os campos e a prioridade da thread principal. |
| `thread_create()` | Herdar `nice` e `recent_cpu` da thread criadora, calcular a prioridade inicial e verificar preempção. |
| `thread_tick()` | Executar atualizações periódicas e solicitar preempção quando necessário. |
| `priority_less()` / `next_thread_to_run()` | Escolher a thread pronta de maior prioridade usando `list_max()`. |
| `thread_set_nice()` / `thread_get_nice()` | Alterar e consultar o valor `nice`; recalcular a prioridade quando necessário. |
| `thread_get_recent_cpu()` / `thread_get_load_avg()` | Retornar as respectivas métricas multiplicadas por 100 e arredondadas. |
| `thread_set_priority()` | Ignorar a definição manual de prioridade quando MLFQS estiver habilitado. |

A *idle thread* não participa dos cálculos de `recent_cpu` nem de `load_avg` como thread executável. Em caso de empate de prioridade na lista de prontas, a seleção preserva a ordem entre os elementos empatados.

## 3. Sincronização e preempção

Foi necessária uma alteração localizada em [`src/threads/synch.c`](src/threads/synch.c) para integrar o MLFQS aos semáforos, especialmente no cenário testado por `mlfqs-block`.

- **`sema_priority_less()`**: compara a prioridade das threads que aguardam um semáforo.
- **`sema_up()`**: no modo MLFQS, seleciona a thread de maior prioridade entre as que aguardam; torna-a pronta; **incrementa `sema->value` antes de solicitar a preempção**; e, quando apropriado, usa `thread_yield()` fora de interrupções ou `intr_yield_on_return()` em contexto de interrupção.

A ordem é importante: a operação do semáforo deve terminar **antes** de a thread despertada voltar a executar. Quando MLFQS está desativado, a seleção em `sema_up()` mantém o comportamento original da fila.

> **Escopo:** o projeto implementa o Alarm Clock e o MLFQS. Não implementa integralmente o escalonamento por prioridade com *priority donation* do projeto original do PintOS.

## 4. Compilação e execução dos testes

É necessário ter o ambiente de compilação do PintOS configurado, incluindo as ferramentas utilizadas pelo projeto para executar o kernel no QEMU.

Na raiz do repositório:

```bash
make -C src/threads
cd src/threads/build
```

### Testes do Alarm Clock

```bash
make tests/threads/alarm-single.result \
     tests/threads/alarm-multiple.result \
     tests/threads/alarm-simultaneous.result \
     tests/threads/alarm-zero.result \
     tests/threads/alarm-negative.result
```

### Testes do Advanced Scheduler

```bash
make tests/threads/mlfqs-load-1.result \
     tests/threads/mlfqs-load-60.result \
     tests/threads/mlfqs-load-avg.result \
     tests/threads/mlfqs-recent-1.result \
     tests/threads/mlfqs-fair-2.result \
     tests/threads/mlfqs-fair-20.result \
     tests/threads/mlfqs-nice-2.result \
     tests/threads/mlfqs-nice-10.result \
     tests/threads/mlfqs-block.result
```

Para consultar os resultados:

```bash
grep -H . tests/threads/mlfqs-*.result \
  tests/threads/alarm-{single,multiple,simultaneous,zero,negative}.result
```

**Observação:** os alvos `.result` podem reutilizar resultados anteriores se já estiverem atualizados. Para executar novamente os testes, remova os `.result` correspondentes antes de chamar `make`.

### Resultados registrados

No snapshot do projeto utilizado para esta documentação, os arquivos de resultado dos testes selecionados apresentam:

| Componente | Testes aprovados |
| --- | ---: |
| Alarm Clock | **5/5** |
| Advanced Scheduler (MLFQS) | **9/9** |
| **Total** | **14/14** |

Esses resultados correspondem aos testes selecionados presentes no repositório; não representam validação dos testes de *priority donation*, de todos os testes de `make check` ou de testes ocultos.

## 5. Referências

- [Documentação do PintOS — Project 1: Threads](https://web.stanford.edu/class/cs140/projects/pintos/pintos_2.html)
- *Pintos*, Ben Pfaff — especialmente o Apêndice B, **4.4BSD Scheduler**.
- *CSCI 350: Pintos Guide*, Stephen Tsung-Han Sher — seção **Project 1: Threads**.
- [Vídeo de referência — Pintos Project 1 (parte 1)](https://youtu.be/myO2bs5LMak)
- [Vídeo de referência — Pintos Project 1 (parte 2)](https://youtu.be/4-OjMqyygss)
rm -f README.md && nano README.md
# PintOS — Alarm Clock e Advanced Scheduler (MLFQS)

Implementação de duas funcionalidades do subsistema de **threads** do [PintOS](https://web.stanford.edu/class/cs140/projects/pintos/pintos.html): **Alarm Clock**, para suspender threads sem espera ocupada, e **Advanced Scheduler (MLFQS)**, baseado no escalonador 4.4BSD, para ajustar prioridades dinamicamente conforme o uso da CPU.

O projeto foi desenvolvido em **C**, sobre a infraestrutura de threads, interrupções e sincronização fornecida pelo PintOS.

## Funcionalidades implementadas

| Funcionalidade | Objetivo | Arquivos envolvidos |
| --- | --- | --- |
| **Alarm Clock** | Substituir a espera ocupada por bloqueio e despertar no tick programado. | [`src/devices/timer.c`](src/devices/timer.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Advanced Scheduler (MLFQS)** | Calcular prioridades dinamicamente usando `nice`, `recent_cpu` e `load_avg`. | [`src/threads/thread.c`](src/threads/thread.c), [`src/threads/thread.h`](src/threads/thread.h) |
| **Integração com semáforos** | Acordar a thread de maior prioridade no modo MLFQS e permitir preempção após liberar o semáforo. | [`src/threads/synch.c`](src/threads/synch.c) |

## 1. Alarm Clock

### Problema original

Na implementação inicial, `timer_sleep()` consultava repetidamente o tempo decorrido e chamava `thread_yield()`. Mesmo sem trabalho útil, a thread permanecia elegível ao escalonamento, caracterizando **busy waiting** (espera ocupada).

### Solução adotada

Em [`src/devices/timer.c`](src/devices/timer.c):

- **`sleeping_list`**: lista de threads adormecidas, ordenada pelo tick de despertar.
- **`timer_init()`**: inicializa a lista de threads adormecidas.
- **`wake_before()`**: compara os instantes de despertar e mantém a ordenação.
- **`timer_sleep()`**: calcula `wake_tick`, insere a thread na lista e chama `thread_block()` com interrupções temporariamente desabilitadas para proteger a operação.
- **`timer_interrupt()`**: incrementa o contador de ticks, retira da lista as threads cujo prazo chegou e as torna prontas novamente com `thread_unblock()`.

Em [`src/threads/thread.h`](src/threads/thread.h), foram adicionados à `struct thread`:

```c
int64_t wake_tick;            /* Tick em que a thread deve acordar. */
struct list_elem sleep_elem;  /* Elemento próprio da lista de sono. */
```

O uso de `sleep_elem` evita conflitos com `elem`, que já é compartilhado entre a lista de prontas e as listas de espera de semáforos.

**Fluxo:** `timer_sleep()` → `THREAD_BLOCKED` → interrupção no tick programado → `thread_unblock()` → `THREAD_READY`.

## 2. Advanced Scheduler — MLFQS (4.4BSD)

O **Multi-Level Feedback Queue Scheduler (MLFQS)** prioriza threads segundo o consumo recente de CPU e o valor `nice`. O modo é ativado pelo argumento de inicialização **`-mlfqs`**. Embora o modelo conceitual descreva 64 níveis de prioridade, esta implementação mantém **uma única `ready_list`** e seleciona o elemento de maior prioridade.

### Informações mantidas pelo escalonador

Em [`src/threads/thread.h`](src/threads/thread.h), foram acrescentados à `struct thread`:

```c
int nice;           /* Ajuste de prioridade da thread. */
int32_t recent_cpu; /* Histórico de CPU em ponto fixo 17.14. */
```

Em [`src/threads/thread.c`](src/threads/thread.c), a variável global estática `load_avg` representa a carga média do sistema. Valores fracionários são manipulados com **aritmética de ponto fixo 17.14** (`2^14 = 16384` como fator de escala), evitando o uso de ponto flutuante no kernel. Multiplicações e divisões de valores em ponto fixo utilizam intermediários de 64 bits.

### Fórmulas utilizadas

**Prioridade dinâmica:**

```text
priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)
```

O resultado é convertido para inteiro e limitado ao intervalo **0 a 63**.

**Uso recente de CPU:**

```text
recent_cpu = ((2 * load_avg) / (2 * load_avg + 1)) * recent_cpu + nice
```

**Carga média:**

```text
load_avg = (59/60) * load_avg + (1/60) * ready_threads
```

`ready_threads` considera as threads prontas e a thread em execução, exceto a *idle thread*.

### Frequência das atualizações

As atualizações são integradas à função **`thread_tick()`**, em `thread.c`:

| Frequência | Operação |
| --- | --- |
| **A cada tick** | Incrementar `recent_cpu` da thread em execução, exceto a idle. |
| **A cada segundo** (`TIMER_FREQ` ticks) | Recalcular `load_avg` e o `recent_cpu` de todas as threads. |
| **A cada 4 ticks** | Recalcular a prioridade de todas as threads. |

### Funções implementadas ou adaptadas em `thread.c`

| Função | Responsabilidade |
| --- | --- |
| `mlfqs_update_priority()` | Recalcular e limitar a prioridade de uma thread. |
| `mlfqs_update_recent_cpu()` | Aplicar a fórmula de consumo recente de CPU. |
| `mlfqs_update_load_avg()` | Atualizar a carga média do sistema. |
| `mlfqs_update_priority_callback()` | Permitir atualizar prioridades usando `thread_foreach()`. |
| `thread_init()` / `init_thread()` | Inicializar os campos e a prioridade da thread principal. |
| `thread_create()` | Herdar `nice` e `recent_cpu` da thread criadora, calcular a prioridade inicial e verificar preempção. |
| `thread_tick()` | Executar atualizações periódicas e solicitar preempção quando necessário. |
| `priority_less()` / `next_thread_to_run()` | Escolher a thread pronta de maior prioridade usando `list_max()`. |
| `thread_set_nice()` / `thread_get_nice()` | Alterar e consultar o valor `nice`; recalcular a prioridade quando necessário. |
| `thread_get_recent_cpu()` / `thread_get_load_avg()` | Retornar as respectivas métricas multiplicadas por 100 e arredondadas. |
| `thread_set_priority()` | Ignorar a definição manual de prioridade quando MLFQS estiver habilitado. |

A *idle thread* não participa dos cálculos de `recent_cpu` nem de `load_avg` como thread executável. Em caso de empate de prioridade na lista de prontas, a seleção preserva a ordem entre os elementos empatados.

## 3. Sincronização e preempção

Foi necessária uma alteração localizada em [`src/threads/synch.c`](src/threads/synch.c) para integrar o MLFQS aos semáforos, especialmente no cenário testado por `mlfqs-block`.

- **`sema_priority_less()`**: compara a prioridade das threads que aguardam um semáforo.
- **`sema_up()`**: no modo MLFQS, seleciona a thread de maior prioridade entre as que aguardam; torna-a pronta; **incrementa `sema->value` antes de solicitar a preempção**; e, quando apropriado, usa `thread_yield()` fora de interrupções ou `intr_yield_on_return()` em contexto de interrupção.

A ordem é importante: a operação do semáforo deve terminar **antes** de a thread despertada voltar a executar. Quando MLFQS está desativado, a seleção em `sema_up()` mantém o comportamento original da fila.

> **Escopo:** o projeto implementa o Alarm Clock e o MLFQS. Não implementa integralmente o escalonamento por prioridade com *priority donation* do projeto original do PintOS.

## 4. Compilação e execução dos testes

É necessário ter o ambiente de compilação do PintOS configurado, incluindo as ferramentas utilizadas pelo projeto para executar o kernel no QEMU.

Na raiz do repositório:

```bash
make -C src/threads
cd src/threads/build
```

### Testes do Alarm Clock

```bash
make tests/threads/alarm-single.result \
     tests/threads/alarm-multiple.result \
     tests/threads/alarm-simultaneous.result \
     tests/threads/alarm-zero.result \
     tests/threads/alarm-negative.result
```

### Testes do Advanced Scheduler

```bash
make tests/threads/mlfqs-load-1.result \
     tests/threads/mlfqs-load-60.result \
     tests/threads/mlfqs-load-avg.result \
     tests/threads/mlfqs-recent-1.result \
     tests/threads/mlfqs-fair-2.result \
     tests/threads/mlfqs-fair-20.result \
     tests/threads/mlfqs-nice-2.result \
     tests/threads/mlfqs-nice-10.result \
     tests/threads/mlfqs-block.result
```

Para consultar os resultados:

```bash
grep -H . tests/threads/mlfqs-*.result \
  tests/threads/alarm-{single,multiple,simultaneous,zero,negative}.result
```

**Observação:** os alvos `.result` podem reutilizar resultados anteriores se já estiverem atualizados. Para executar novamente os testes, remova os `.result` correspondentes antes de chamar `make`.

### Resultados registrados

No snapshot do projeto utilizado para esta documentação, os arquivos de resultado dos testes selecionados apresentam:

| Componente | Testes aprovados |
| --- | ---: |
| Alarm Clock | **5/5** |
| Advanced Scheduler (MLFQS) | **9/9** |
| **Total** | **14/14** |

Esses resultados correspondem aos testes selecionados presentes no repositório; não representam validação dos testes de *priority donation*, de todos os testes de `make check` ou de testes ocultos.

## 5. Referências

- [Documentação do PintOS — Project 1: Threads](https://web.stanford.edu/class/cs140/projects/pintos/pintos_2.html)
- *Pintos*, Ben Pfaff — especialmente o Apêndice B, **4.4BSD Scheduler**.
- *CSCI 350: Pintos Guide*, Stephen Tsung-Han Sher — seção **Project 1: Threads**.
- [Vídeo de referência — Pintos Project 1 (parte 1)](https://youtu.be/myO2bs5LMak)
- [Vídeo de referência — Pintos Project 1 (parte 2)](https://youtu.be/4-OjMqyygss)


