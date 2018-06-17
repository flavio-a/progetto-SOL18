/**
 * @file hashtable.c
 * @brief Implementazione di hashtable.h
 */

#include "hashtable.h"

// ------------------ Funzioni interne ---------------


// ------- Funzioni esportate --------------
// Documentate in hashtable.h

htable_t* hash_create(int nbuckets, int history_size) {
	htable_t* res = malloc(sizeof(htable_t));
	res->htable = icl_hash_create(nbuckets, NULL, NULL);
	res->hist_size = history_size;
	pthread_mutex_init(&(res->mutex), NULL);

	return res;
}

int ts_hash_destroy(htable_t* ht) {
	// TODO (opt): togliere la sincronizzazione
	error_handling_lock(&(ht->mutex));
	int res = icl_hash_destroy(ht->htable, &free, &free_nickname_t);
	if (res == 0)
		free(ht);
	error_handling_unlock(&(ht->mutex));
	pthread_mutex_destroy(&(ht->mutex));
	return res;
}

nickname_t* ts_hash_find(htable_t* ht, char* key) {
	return (nickname_t*)icl_hash_find(ht->htable, (void*)key);
}

nickname_t* ts_hash_insert(htable_t* ht, char* key) {
	nickname_t* val = create_nickname(ht->hist_size);
	char* new_key = malloc((strlen(key) + 1) * sizeof(char*));
	strncpy(new_key, key, strlen(key) + 1);
	error_handling_lock(&(ht->mutex));
	icl_entry_t* res = icl_hash_insert(ht->htable, new_key, val);
	error_handling_unlock(&(ht->mutex));
	return res == NULL ? NULL : res->data;
}

bool ts_hash_remove(htable_t* ht, char* key) {
	error_handling_lock(&(ht->mutex));
	int res = icl_hash_delete(ht->htable, (void*)key, &free, &free_nickname_t);
	error_handling_unlock(&(ht->mutex));
	return res == 0;
}
