/**
 * @file nickname.c
 * @brief Implementazione di nickname.h
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#include "nickname.h"

// ------------------ Funzioni interne ---------------

// ------- Funzioni esportate --------------
// Documentate in nickname.h

nickname_t* create_nickname(int history_size) {
	nickname_t* res = malloc(sizeof(nickname_t));
	res->fd = 0;
	res->first = -1;
	res->hist_size = history_size;
	res->history = malloc(history_size * sizeof(message_t));
	// questo segnala se l'ultimo messaggio è stato mai inizializzato o meno
	res->history[history_size - 1].hdr.op = OP_FAKE_MSG;
	pthread_mutex_init(&(res->mutex), NULL);
	return res;
}

void free_nickname(void* val) {
	nickname_t* tmp = (nickname_t*)val;
	error_handling_lock(&(tmp->mutex));
	// Free della history
	int i;
	message_t* msg;
	history_foreach(tmp, i, msg) {
		free(msg->data.buf);
	}
	free(tmp->history);
	error_handling_unlock(&(tmp->mutex));
	pthread_mutex_destroy(&(tmp->mutex));
	free(tmp);
}

bool is_history_full(nickname_t* nick) {
	return nick->history[nick->hist_size - 1].hdr.op != OP_FAKE_MSG;
}

void add_to_history(nickname_t* nick, message_t msg) {
	// aggiunta alla coda circolare: aumento l'indice di testa e sostituisco
	nick->first = ((nick->first) + 1) % nick->hist_size;
	if (is_history_full(nick)) {
		// devo liberare la memoria occupata dal vecchio messaggio
		if (nick->history[nick->first].data.buf != NULL) {
			#if defined DEBUG && defined VERBOSE
				fprintf(stderr, "HTABLE: Libero il buffer sovrascrivendo la history %p\n", nick->history[nick->first].data.buf);
			#endif
			free(nick->history[nick->first].data.buf);
		}
	}
	nick->history[nick->first] = msg;
}

int history_len(nickname_t* nick) {
	if (is_history_full(nick))
		return nick->hist_size;
	else
		return nick->first + 1;
}
