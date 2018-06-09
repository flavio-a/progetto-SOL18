/**
 * @file hashtable.c
 * @brief Implementazione di hashtable.h
 */

#include "hashtable.h"

// ------------------ Funzioni interne ---------------



// ------- Funzioni esportate --------------
// Documentate in hashtable.h

htable_t* hash_create(int nbuckets) {
	htable_t* res = malloc(sizeof(htable_t));

	res->htable = icl_hash_create(nbuckets, NULL, NULL);
	res->reader_in = res->writer_in = res->reader_wait = res->writer_wait = 0;

	pthread_mutex_init(&(res->mutex), NULL);
	pthread_cond_init(&(res->cond_reader), NULL);
	pthread_cond_init(&(res->cond_writer), NULL);

	return res;
}

int ts_hash_destroy(htable_t* ht) {
	error_handling_lock(&(ht->mutex));
	int res = icl_hash_destroy(ht->htable, &free, NULL);
	error_handling_unlock(&(ht->mutex));
	if (res == 0)
		free(ht);
	return res;
}

// lettore
bool ts_hash_find(htable_t* ht, char* key) {
	// prologo dell'accesso in lettura
	error_handling_lock(&(ht->mutex));
	if (ht->writer_in == 0 && ht->writer_wait == 0) {
		(ht->reader_in)++;
		error_handling_unlock(&(ht->mutex));
	}
	else {
 		(ht->reader_wait)++;
		while (ht->writer_in > 0)
			pthread_cond_wait(&(ht->cond_reader), &(ht->mutex));
		(ht->reader_wait)--;
		(ht->reader_in)++;
		error_handling_unlock(&(ht->mutex));
	}

	// lettura
	void* val = icl_hash_find(ht->htable, (void*)key);

	// epilogo dell'accesso in lettura
	error_handling_lock(&(ht->mutex));
	(ht->reader_in)--;
	if (ht->reader_in == 0) {
		if (ht->writer_wait > 0 ) {
			pthread_cond_signal(&(ht->cond_writer));
		}
		else {
			// in teoria non serve mai a nente, messo per sicurezza
			pthread_cond_broadcast(&(ht->cond_reader));
		}
	}
	error_handling_unlock(&(ht->mutex));

	return val != NULL;
}

// scrittore
bool ts_hash_insert(htable_t* ht, char* key) {
	bool* val = malloc(sizeof(bool));
	*val = true;

	// prologo dell'accesso in lettura
	error_handling_lock(&(ht->mutex));
	if (ht->reader_in == 0 && ht->writer_in == 0 && ht->reader_wait == 0) {
		(ht->writer_in)++;
		error_handling_unlock(&(ht->mutex));
	}
	else {
	 	(ht->writer_wait)++;
		while (ht->reader_in > 0 || ht->writer_in > 0)
			pthread_cond_wait(&(ht->cond_writer), &(ht->mutex));
		(ht->writer_wait)--;
		(ht->writer_in)++;
		error_handling_unlock(&(ht->mutex));
	}

	// scrittura
	void* res = icl_hash_insert(ht->htable, (void*)key, val);

	// epilogo dell’accesso in scrittura
	error_handling_lock(&(ht->mutex));
	(ht->writer_in)--;
	if (ht->reader_wait > 0) {
		pthread_cond_broadcast(&(ht->cond_reader));
	}
	else {
		pthread_cond_signal(&(ht->cond_writer));
	}
	error_handling_unlock(&(ht->mutex));

	return res == NULL;
}

// scrittore
bool ts_hash_remove(htable_t* ht, char* key) {
	// prologo dell'accesso in scrittura
	error_handling_lock(&(ht->mutex));
	if (ht->reader_in == 0 && ht->writer_in == 0 && ht->reader_wait == 0) {
		(ht->writer_in)++;
		error_handling_unlock(&(ht->mutex));
	}
	else {
	 	(ht->writer_wait)++;
		while (ht->reader_in > 0 || ht->writer_in > 0)
			pthread_cond_wait(&(ht->cond_writer), &(ht->mutex));
		(ht->writer_wait)--;
		(ht->writer_in)++;
		error_handling_unlock(&(ht->mutex));
	}

	// scrittura
	int res = icl_hash_delete(ht->htable, (void*)key, &free, &free);

	// epilogo dell’accesso in scrittura
	error_handling_lock(&(ht->mutex));
	(ht->writer_in)--;
	if (ht->reader_wait > 0) {
		pthread_cond_broadcast(&(ht->cond_reader));
	}
	else {
		pthread_cond_signal(&(ht->cond_writer));
	}
	error_handling_unlock(&(ht->mutex));

	return res == 0;
}
