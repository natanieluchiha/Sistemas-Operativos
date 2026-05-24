#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sthread.h>
#include <sthread_user.h>   
#include "queue.h"


#define NUM_CLIENTES 10
#define NUM_CAIXAS 2

// Monitor global
sthread_mon_t monitor;

// Filas e contadores
int fila[NUM_CAIXAS] = {0};
queue_t* espera[NUM_CAIXAS];

int sthread_get_tid(struct _sthread *thr);/*Funcao getter*/


// Retorna o índice da fila com menos clientes
int escolher_fila() {
    return (fila[0] <= fila[1]) ? 0 : 1;
}

void SerAtendido(int tempo) {
    sthread_sleep(tempo); // Simula tempo de atendimento
    sthread_sleep(30);    // Espera antes de ir embora
}

void Atender(int tempo) {
    sthread_sleep(tempo); // Simula tempo do atendimento
}

void *cliente(void *arg) {
    int id = *(int *)arg;
    free(arg);

    int minha_fila;

    sthread_monitor_enter(monitor);
    minha_fila = escolher_fila();
    fila[minha_fila]++;

    extern struct _sthread* active_thr;
    queue_insert(espera[minha_fila], active_thr);

    printf("Cliente %d entrou na fila %d\n", id, minha_fila);
    sthread_monitor_wait(monitor); // Espera ser atendido
    sthread_monitor_exit(monitor);

    SerAtendido(50 + id * 5);

    sthread_exit(NULL);
    return NULL;
}

void *funcionario(void *arg) {
    int id = *(int *)arg;
    free(arg);

    while (1) {
        sthread_monitor_enter(monitor);

        // Tenta encontrar um cliente na própria fila ou na outra
        int alvo = -1;
        if (!queue_is_empty(espera[id])) {
            alvo = id;
        } else if (!queue_is_empty(espera[1 - id])) {
            alvo = 1 - id;
        }

        if (alvo != -1) {
            struct _sthread *cliente = queue_remove(espera[alvo]);
            fila[alvo]--;
            printf("Funcionario %d vai atender cliente %d da fila %d\n", id, sthread_get_tid(cliente), alvo);
            sthread_monitor_signal(monitor); // Desbloqueia o cliente
            sthread_monitor_exit(monitor);
            Atender(80);
        } else {
            // Nada a fazer, bloqueia
            sthread_monitor_wait(monitor);
            sthread_monitor_exit(monitor);
        }
    }
}

int main() {
    sthread_init();
    monitor = sthread_monitor_init();
    espera[0] = create_queue();
    espera[1] = create_queue();

    // Cria funcionários
    for (int i = 0; i < NUM_CAIXAS; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        sthread_create(funcionario, id, 2);
    }

    // Cria clientes
    for (int i = 0; i < NUM_CLIENTES; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        sthread_create(cliente, id, 7);
    }

    sthread_yield();
    return 0;
}
