/**
 * @brief Test per il file fifo.h
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "fifo.h"

#define M 30
#define N 100
#define K 100000
#define MAX_MSG 10000

static fifo_t buffer;
static int k = K, k1 = K;
static pthread_mutex_t mutex_k, mutex_n, mutex_k1;

static int prodotti[K], consumati[K];

void* producer(void* arg){
	int v;

	pthread_mutex_lock(&mutex_k);
	while (k-- > 0) {
		v = rand() % MAX_MSG;
		// printf("Prodotto %d\n", v);
		prodotti[k] = v;
		pthread_mutex_unlock(&mutex_k);
		ts_push(&buffer, v);
		pthread_mutex_lock(&mutex_k);
	}
	pthread_mutex_unlock(&mutex_k);

	return NULL;
}

void* consumer(void* arg){
	int v;
	while (1) {
		v = ts_pop(&buffer);
		// printf("Consumato %d\n", v);
		pthread_mutex_lock(&mutex_k1);
		consumati[--k1] = v;
		pthread_mutex_unlock(&mutex_k1);
	}

	return NULL;
}

int cmpfunc(const void * a, const void * b) {
	return *(int*)a - *(int*)b;
}

int main(int argc, char** argv) {
	buffer = create_fifo();
	pthread_mutex_init(&mutex_k, NULL);
	pthread_mutex_init(&mutex_n, NULL);
	srand(time(NULL));

	pthread_t tid[M];
	pthread_t tid_temp;
	for (int i = 0; i < M; ++i) {
		pthread_create(tid + i, NULL, &producer, NULL);
	}
	for (int i = 0; i < N; ++i) {
		pthread_create(&tid_temp, NULL, &consumer, NULL);
	}

	for (int i = 0; i < M; ++i)
		pthread_join(tid[i], NULL);
	// questa soluzione non è thread safe, ma va benissimo per fare il test
	sleep(3);

	// controllo del risultato
	printf("Inizio controllo del risultato\n");
	if (!ts_is_empty(buffer))
		fprintf(stderr, "Errore: %d rimasto nel buffer\n", ts_pop(&buffer));
	qsort(prodotti, K, sizeof(int), cmpfunc);
	qsort(consumati, K, sizeof(int), cmpfunc);
	for (int i = 0; i < K; ++i){
		// fprintf(stdout, "%d   %d\n", prodotti[i], consumati[i]);
		if (prodotti[i] != consumati[i]) {
			fprintf(stderr, "ERROR\n");
			exit(EXIT_FAILURE);
		}
	}

	return 0;
}
