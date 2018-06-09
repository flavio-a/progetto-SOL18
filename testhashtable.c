#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "hashtable.h"

#define N 100
#define K 100000
#define MAX_MSG 100000


static htable_t buffer;
static int k = 0, k1 = 0;
static pthread_mutex_t mutex_k, mutex_k1;

static int prodotti[K];

void* producer(void* arg){
	int v;
	char* tmp;

	pthread_mutex_lock(&mutex_k);
	while (k < K) {
		v = rand() % MAX_MSG;
		// printf("Prodotto %d\n", v);
		prodotti[k] = v;
		pthread_mutex_unlock(&mutex_k);
		tmp = malloc(7 * sizeof(char));
		sprintf(tmp, "%d", v);
		ts_hash_insert(&buffer, tmp);
		pthread_mutex_lock(&mutex_k);
		k++;
	}
	pthread_mutex_unlock(&mutex_k);

	return NULL;
}

void* consumer(void* arg){
	int v;
	char* cusu;

	pthread_mutex_lock(&mutex_k1);
	while (k1 < K) {
		v = prodotti[k1];
		pthread_mutex_unlock(&mutex_k1);
		cusu = malloc(7 * sizeof(char));
		sprintf(cusu, "%d", v);
		if (!ts_hash_find(&buffer, cusu)) {
			fprintf(stderr, "Non trovato %d\n", v);
			exit(EXIT_FAILURE);
		}
		// printf("Controllato %d\n", v);
		pthread_mutex_lock(&mutex_k1);
		k1++;
	}
	pthread_mutex_unlock(&mutex_k1);

	return NULL;
}

int main(int argc, char** argv) {
	buffer = *hash_create(K * 2);
	pthread_mutex_init(&mutex_k, NULL);
	pthread_mutex_init(&mutex_k1, NULL);
	srand(time(NULL));

	pthread_t tid[N];
	// riempie l'hashtable
	for (int i = 0; i < N; ++i) {
		pthread_create(tid + i, NULL, &producer, NULL);
	}
	for (int i = 0; i < N; ++i) {
		pthread_join(tid[i], NULL);
	}

	printf("Hashtable riempita\n");
	// controlla che nell'hashtable ci sia tutto
	for (int i = 0; i < N; ++i) {
		pthread_create(tid + i, NULL, &consumer, NULL);
	}
	for (int i = 0; i < N; ++i) {
		pthread_join(tid[i], NULL);
	}

	printf("Inizio prova di rimozione\n");
	fprintf(stderr, "NON ANCORA IMPLEMENTATA!\n");

	// se ci fossero stati problemi il thread che li ha trovati avrebbe
	// terminato il processo con EXIT_FAILURE
	return 0;
}
