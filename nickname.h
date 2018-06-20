/**
 * @file nickname.h
 * @brief Libreria per la struttura dati nickname_t, elementi dell'hashtable dei
          nickname.
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#ifndef CHATTERBOX_NICKNAME_H_
#define CHATTERBOX_NICKNAME_H_

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>

#include "lock.h"
#include "message.h"

/**
 * @struct nickname
 * @brief Questa struttura dati contiene tutte le informazioni relative ad un
 * nickname.
 *
 * La history viene gestita con una coda circolare, da scorrere all'indietro per
 * avere i messaggi in ordine cronologico (dal più nuovo). Per controllare se
 * la coda circolare è piena, l'ultimo elemento viene inizializzato con una
 * operazione apposta, OP_FAKE_MSG: quando la coda viene riempita per la prima
 * volta, l'ultimo elemento viene sovrascritto e la sua operazione diventa
 * regolare. La variabile first contiene l'indice dell'ultimo elemento inserito,
 * e viene aumentata (modulo hist_size) ogni volta che si vuole aggiungere un
 * nuovo elemento.
 *
 * @var struct nickname::fd Il fd su cui è aperta la connessione con il client
 *                          connesso con quel nickname. Se fd è 0 vuol dire che
 *                          nessun client è connesso con quel nickname
 * @var struct nickname::first Indice di inizio della coda circolare
 *                             dell'history
 * @var struct nickname::hist_size Dimensione dell'history
 * @var struct nickname::history Array di messaggi che rappresentano la history
 */
typedef struct nickname {
	int fd, first, hist_size;
	message_t* history;
	pthread_mutex_t mutex;
} nickname_t;

/**
 * @brief Itera su tutta l'history. Si aspetta che il lock su nick->mutex sia
 * già stato acquisito.
 *
 * Questa macro funziona pensando l'array circolare come replicato dagli indici
 * [0; hist_size) e [hist_size; 2 * hist_size), e scorre da (first + hist_size)
 * a (first + 1) scendendo. Se la history non è piena si ferma a hist_size.
 *
 * \image html immagine_history_foreach.png
 *
 * @param nick (nickname_t*) Il nickname sulla cui history si vuole iterare.
 * @param i (int) L'indice nella history, va usato sempre modulo hist_size (si
                  consiglia di usare direttamente l'elemento msg invece di i).
 * @param msg (message_t*) Puntatore all'elemento corrente della history.
 */
#define history_foreach(nick, i, msg) \
	for(msg = &(nick->history[(i = nick->first + nick->hist_size) % nick->hist_size]); \
		i > (is_history_full(nick) ? nick->first : nick->hist_size - 1); \
		msg = &(nick->history[(--i) % nick->hist_size]))


/**
 * @brief Inizializza un nuovo nickname_t
 *
 * @param history_size La dimensione dell'history di questo nickname.
 * @return Il puntatore al nuovo nickname_t
 */
nickname_t* create_nickname(int history_size);


/**
 * @brief Elimina un nickname_t, liberando tutta la memoria che aveva allocato.
 * Questa funzione si occupa anche di liberare la memoria occupata dalla
 * history e dai messaggi che conteneva.
 *
 * Il parametro è di tipo void* per evitare warnings quando viene passata ad
 * icl_hash_remove e icl_hash_destroy.
 *
 * @param val (nickname_t*) il nickname da eliminare.
 */
void free_nickname(void* val);

/**
 * @brief Controlla se l'history di un nickname_t è piena.
 *
 * @param nick Il nickname su cui controllare.
 * @return True se l'history è piena, false altrimenti.
 */
bool is_history_full(nickname_t* nick);

/**
 * @brief Aggiunge un messaggio alla history. Libera la memoria quando serve.
 *
 * Si aspetta che sia già stato acquisito il lock su nick->mutex.
 *
 * @param nick Il nickname_t a cui aggiungere il messaggio
 * @param msg Il messaggio da aggiungere (il messaggio viene copiato, il
              il puntatore al buffer però rimane lo stesso)
 */
void add_to_history(nickname_t* nick, message_t msg);

/**
 * @brief Calcola la lunghezza della history. Si aspetta che il lock su
 * nick->mutex sia già stato acquisito.
 *
 * @param nick Il nickname_t di cui calcolare la lunghezza della history.
 * @return La lunghezza della history.
 */
int history_len(nickname_t* nick);

#endif /* CHATTERBOX_NICKNAME_H_ */
