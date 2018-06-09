#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "hashtable.h"

#define N 100
#define K 100000
#define MSG_LEN 10
#define MAX_MSG 100000000


static htable_t buffer;
static int k = 0, kprod = 0, kcons = 0;
static pthread_mutex_t mutex_k;

static int prodotti[K];

void* producer(void* arg) {
	int v;
	char* tmp;

	pthread_mutex_lock(&mutex_k);
	while (k < K) {
		k++;
		pthread_mutex_unlock(&mutex_k);
		v = rand() % MAX_MSG;
		// printf("Prodotto %d\n", v);
		tmp = malloc(MSG_LEN * sizeof(char));
		sprintf(tmp, "%d", v);
		ts_hash_insert(&buffer, tmp);
		pthread_mutex_lock(&mutex_k);
		prodotti[kprod] = v;
		kprod++;
	}
	pthread_mutex_unlock(&mutex_k);

	return NULL;
}

void* consumer(void* arg) {
	int v;
	char* cusu;

	pthread_mutex_lock(&mutex_k);
	while (kcons < kprod) {
		v = prodotti[kcons];
		kcons++;
		pthread_mutex_unlock(&mutex_k);
		cusu = malloc(MSG_LEN * sizeof(char));
		sprintf(cusu, "%d", v);
		if (!ts_hash_find(&buffer, cusu)) {
			fprintf(stderr, "Non trovato %d\n", v);
			exit(EXIT_FAILURE);
		}
		// printf("Controllato %d\n", v);
		pthread_mutex_lock(&mutex_k);
	}
	pthread_mutex_unlock(&mutex_k);

	return NULL;
}

int cmpfunc(const void * a, const void * b) {
	return *(int*)a - *(int*)b;
}

int main(int argc, char** argv) {
	buffer = *hash_create(K * 2);
	pthread_mutex_init(&mutex_k, NULL);
	srand(time(NULL));

	pthread_t tidP[N], tidC[N];
	printf("Inizio prova di inserimenti/ricerce concorrenti\n");
	// riempie l'hashtable
	for (int i = 0; i < N; ++i) {
		pthread_create(tidP + i, NULL, &producer, NULL);
	}
	// per provare a causare un po' di concorrenza tra produttori e consumatori
	sched_yield();
	for (int i = 0; i < N; ++i) {
		pthread_create(tidC + i, NULL, &consumer, NULL);
	}
	for (int i = 0; i < N; ++i) {
		pthread_join(tidP[i], NULL);
	}
	for (int i = 0; i < N; ++i) {
		pthread_join(tidC[i], NULL);
	}
	// per essere sicuro che nell'hashtable ci sia tutto, spawno un consumer
	// dopo la fine di tutti i prducer
	pthread_create(tidC, NULL, &consumer, NULL);
	pthread_join(tidC[0], NULL);

	printf("Inizio prova di rimozione\n");
	// la concorrenza di lettori/scrittori è già stata testata, il test delle
	// rimozioni può essere sequenziale
	char* cusu;
	// prima della rimozione li ordina per sistemare i problemi con i doppi
	qsort(prodotti, K, sizeof(int), cmpfunc);
	// rimozione
	for (int i = 0; i < K / 2; ++i) {
		cusu = malloc(MSG_LEN * sizeof(char));
		sprintf(cusu, "%d", prodotti[i]);
		ts_hash_remove(&buffer, cusu);
		// printf("Rimosso %d\n", prodotti[i]);
	}
	// controlla che non ci siano quelli rimossi
	for (int i = 0; i < K / 2; ++i) {
		cusu = malloc(MSG_LEN * sizeof(char));
		sprintf(cusu, "%d", prodotti[i]);
		if (ts_hash_find(&buffer, cusu)) {
			fprintf(stderr, "Trovato %d\n", prodotti[i]);
			exit(EXIT_FAILURE);
		}
		// printf("Controllato %d\n", prodotti[i]);
	}
	// controlla che ci siano ancora quelli non rimossi
	for (int i = K / 2; i < K; ++i) {
		if (prodotti[i] == prodotti[K / 2 - 1])
			continue;
		cusu = malloc(MSG_LEN * sizeof(char));
		sprintf(cusu, "%d", prodotti[i]);
		if (!ts_hash_find(&buffer, cusu)) {
			fprintf(stderr, "Non trovato %d, indice %d\n", prodotti[i], i);
			exit(EXIT_FAILURE);
		}
		// printf("Controllato %d\n", prodotti[i]);
	}

	// se ci fossero stati problemi il thread che li ha trovati avrebbe
	// terminato il processo con EXIT_FAILURE
	return 0;
}
