/**
 * @file hashtable.c
 * @brief Implementazione di hashtable.h
 */

#include "hashtable.h"

// ------------------ Funzioni interne ---------------

// ============================= nickname_t ====================================
nickname_t* create_nickname(int history_size) {
	nickname_t* res = malloc(sizeof(nickname_t));
	res->fd = 0;
	res->first = 0;
	res->hist_size = history_size;
	res->history = malloc(history_size * sizeof(message_t));
	// questo segnala se l'ultimo messaggio è stato mai inizializzato o meno
	res->history[history_size - 1].hdr.op = OP_FAKE_MSG;
	pthread_mutex_init(&(res->mutex), NULL);
	return res;
}

void free_nickname_t(void* val) {
	error_handling_lock(&(((nickname_t*)val)->mutex));
	// Free del contenuto dei messaggi
	nickname_t tmp = *(nickname_t*)val;
	for (int i = tmp.first; i >= 0; --i) {
		free(tmp.history[i].data.buf);
	}
	if (tmp.history[tmp.hist_size - 1].hdr.op != OP_FAKE_MSG) {
		for (int i = tmp.hist_size - 1; i >= tmp.first; --i) {
			free(tmp.history[i].data.buf);
		}
	}
	free(tmp.history);
	error_handling_unlock(&(tmp.mutex));
	pthread_mutex_destroy(&(tmp.mutex));
	free((nickname_t*)val);
}

// ------- Funzioni esportate --------------
// Documentate in hashtable.h

// ============================= nickname_t ====================================
void add_to_history(nickname_t* nick, message_t msg) {
	// aggiunta alla coda circolare: aumento l'indice di testa e sostituisco
	nick->first = (nick->first) + 1 % nick->hist_size;
	nick->history[nick->first] = msg;
}

bool search_file_history(nickname_t* nick, char* name) {
	for (int i = nick->first; i >= 0; --i) {
		if (nick->history[i].hdr.op == POSTFILE_OP
			&& strncmp(nick->history[i].data.buf, name, nick->history[i].data.hdr.len) == 0)
			return true;
	}
	if (nick->history[nick->hist_size - 1].hdr.op != OP_FAKE_MSG) {
		for (int i = nick->hist_size - 1; i >= nick->first; --i) {
			if (nick->history[i].hdr.op == POSTFILE_OP
				&& strncmp(nick->history[i].data.buf, name, nick->history[i].data.hdr.len) == 0)
				return true;
		}
	}
	return false;
}

// ============================== htable_t =====================================

htable_t* hash_create(int nbuckets, int history_size) {
	htable_t* res = malloc(sizeof(htable_t));
	res->htable = icl_hash_create(nbuckets, NULL, NULL);
	res->hist_size = history_size;
	pthread_mutex_init(&(res->mutex), NULL);

	return res;
}

int ts_hash_destroy(htable_t* ht) {
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
	char* new_key = malloc(strlen(key) * sizeof(char*));
	strncpy(new_key, key, strlen(key));
	error_handling_lock(&(ht->mutex));
	icl_entry_t* res = icl_hash_insert(ht->htable, new_key, val);
	error_handling_unlock(&(ht->mutex));
	return res == NULL ? NULL : res->data;
}

// scrittore
bool ts_hash_remove(htable_t* ht, char* key) {
	error_handling_lock(&(ht->mutex));
	int res = icl_hash_delete(ht->htable, (void*)key, &free, &free_nickname_t);
	error_handling_unlock(&(ht->mutex));
	return res == 0;
}
