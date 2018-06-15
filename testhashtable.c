#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

#include "hashtable.h"

#define ITEMS 1000
#define MAX_KEYS_LENGTH 4
#define HIST_SIZE 1
#define TEST_KEY "cusu"

int main(int argc, char** argv) {
	htable_t* ht = hash_create(2 * ITEMS, HIST_SIZE);

	// inserimento
	nickname_t* insert_item = ts_hash_insert(ht, TEST_KEY);
	// ricerca
	nickname_t* find_item = ts_hash_find(ht, TEST_KEY);
	assert(insert_item == find_item);
	assert(find_item != NULL);
	fprintf(stderr, "fd: %d, first: %d, hist_size: %d\n", find_item->fd, find_item->first, find_item->hist_size);
	assert(find_item->fd == 0 && find_item->first == 0 && find_item->hist_size == HIST_SIZE);
	// inserimento di chiave già esistente
	assert(ts_hash_insert(ht, TEST_KEY) == NULL);
	// rimozione
	ts_hash_remove(ht, TEST_KEY);
	assert(ts_hash_find(ht, TEST_KEY) == NULL);

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
