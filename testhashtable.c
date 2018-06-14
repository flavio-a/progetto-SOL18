#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "hashtable.h"

#define ITEMS 100
#define MAX_KEYS_LENGTH 3
#define HIST_SIZE 1

int main(int argc, char** argv) {
	htable_t* ht = hash_create(2 * ITEMS, HIST_SIZE);

	// prime prove
	char* key1 = malloc(5 * sizeof(char));
	strncpy(key1, "cusu", 5);
	ts_hash_insert(ht, key1);
	nickname_t* cusu_item = ts_hash_find(ht, key1);
	assert(cusu_item != NULL);
	fprintf(stderr, "fd: %d, first: %d, hist_size: %d\n", cusu_item->fd, cusu_item->first, cusu_item->hist_size);
	assert(cusu_item->fd == 0 && cusu_item->first == 0 && cusu_item->hist_size == HIST_SIZE);
	ts_hash_remove(ht, key1);
	assert(ts_hash_find(ht, key1) == NULL);

	printf("Superati test di base\n");

	// inserimento di 100 elementi
	for (int i = 0; i < ITEMS; ++i) {
		char* key = malloc((MAX_KEYS_LENGTH + 1) * sizeof(char));
		snprintf(key, MAX_KEYS_LENGTH + 1, "%d", i);
		ts_hash_insert(ht, key);
		nickname_t* tmp = ts_hash_find(ht, key);
		tmp->fd = i % 2 == 0 ? 0 : i;
	}
	int i;
	icl_entry_t* j;
	char* key;
	nickname_t* val;
	icl_hash_foreach((ht->htable), i, j, key, val) {
		fprintf(stderr, "Iterazione: %d. Chiave: %s. fd: %d\n", i, key, val->fd);
		if (val->fd == 0)
			val->first = 1;
	}
	assert(ts_hash_find(ht, "2")->first == 1);

	printf("Superato test sull'iterazione\n");

	// se ci fossero stati problemi il processo sarebbe già terminato con EXIT_FAILURE
	return 0;
}
