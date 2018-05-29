/**
 * @file hashtable.c
 * @brief Implementazione di hashtable.h
 */

#include "hashtable.h"

// ------------------ Funzioni interne ---------------



// ------- Funzioni esportate --------------
// Documentate in hashtable.h

htable_t* ts_hash_create(int nbuckets) {
	htable_t* res = malloc(sizeof(htable_t));

	res->htable = icl_hash_create(nbuckets);
	res->reader = res->writer = res->wait_reader = res->wait_writer = 0;

	pthread_mutex_init(&(res->mutex), NULL);
	pthread_cond_init(&(res->cond_read), NULL);
	pthread_cond_init(&(res->cond_write), NULL);

	return res;
}

int ts_hash_destroy(htable_t* ht) {
	error_handling_lock(&(ht->mutex));
	int res = icl_hash_destroy(&(ht->htable), &free, NULL);
	error_handling_unlock(&(ht->mutex));
	if (res == 0)
		free(ht);
	return res;
}

// lettore
bool ts_hash_find(htable_t* ht, KEY_T key) {
	error_handling_lock(&(ht->mutex));
	if (ht->wait_writer == 0 && ht->writer == 0) {
		(ht->reader)++;
	}
	else {
 		(ht->wait_reader)++;
		while (ht->writer > 0)
			pthread_cond_wait(&(ht->cond_read), &(ht->mutex));
	}
	error_handling_unlock(&(ht->mutex));

	// lettura
	void* val = icl_hash_find(&(ht->htable), &key);

	error_handling_lock(&(ht->mutex));
	(ht->reader)--;
	if (ht->reader == 0 && ht->wait_writer > 0 ) {
		pthread_cond_signal(&(ht->cond_write));
	}
	error_handling_unlock(&(ht->mutex));

	return val == NULL ? false : true;
}

// scrittore
bool ts_hash_insert(htable_t* ht, KEY_T* key) {
	bool val = true;

	error_handling_lock(&(ht->mutex));
	if (ht->reader > 0 || ht->writer > 0) {
	 	(ht->wait_writer)++;
		while (ht->reader > 0 || ht->writer > 0)
			pthread_cond_wait(&(ht->cond_write), &(ht->mutex));
		(ht->wait_writer)--;
	}
	ht->writer = 1;
	error_handling_unlock(&(ht->mutex));

	// scrittura
	void* res = icl_hash_insert(&(ht->htable), key, &val);

	// epilogo dell’accesso in scrittura//
	error_handling_lock(&(ht->mutex));
	ht->writer = 0;
	if (ht->wait_reader > 0) {
		ht->reader = ht->wait_reader;
		ht->wait_reader = 0;
		pthread_cond_broadcast(&(ht->cond_read));
	}
	else if (ht->wait_writer > 0) {
		pthread_cond_signal(&(ht->cond_write));
	}
	error_handling_unlock(&(ht->mutex));

	return res == NULL ? true : false;
}

// scrittore
bool ts_hash_delete(htable_t* ht, KEY_T* key) {
	bool val = true;

	error_handling_lock(&(ht->mutex));
	if (ht->reader > 0 || ht->writer > 0) {
		(ht->wait_writer)++;
		while (ht->reader > 0 || ht->writer > 0)
			pthread_cond_wait(&(ht->cond_write), &(ht->mutex));
		(ht->wait_writer)--;
	}
	ht->writer = 1;
	error_handling_unlock(&(ht->mutex));

	// scrittura
	int res = icl_hash_delete(&(ht->htable), key, &free, &free);

	// epilogo dell’accesso in scrittura//
	error_handling_lock(&(ht->mutex));
	ht->writer = 0;
	if (ht->wait_reader > 0) {
		ht->reader = ht->wait_reader;
		ht->wait_reader = 0;
		pthread_cond_broadcast(&(ht->cond_read));
	}
	else if (ht->wait_writer > 0) {
		pthread_cond_signal(&(ht->cond_write));
	}
	error_handling_unlock(&(ht->mutex));

	return res == 0 ? true : false;
}
