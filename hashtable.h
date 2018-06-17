/**
 * @file hashtable.h
 * @brief Libreria per l'hashtable condivisa
 */
#ifndef CHATTERBOX_HASH_H_
#define CHATTERBOX_HASH_H_

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "icl_hash.h"
#include "lock.h"
#include "message.h"
#include "ops.h"

// ============================= nickname_t ====================================

/**
 * @struct nickname
 * @brief Questa struttura dati contiene tutte le informazioni relative ad un
 * nickname. La history viene gestita con una coda circolare.
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
 * @param nick (nickname_t*) Il nickname sulla cui history si vuole iterare.
 * @param i (int) L'indice nella history, va usato sempre modulo hist_size.
 * @param msg (message_t*) Puntatore all'elemento corrente della history.
 */
#define history_foreach(nick, i, msg) \
	for(msg = &(nick->history[(i = nick->first + nick->hist_size) % nick->hist_size]); \
		i > (is_history_full(nick) ? nick->first : nick->hist_size - 1); \
		msg = &(nick->history[(--i) % nick->hist_size]))

/**
 * @brief Controlla se l'history di un nickname_t è piena.
 *
 * @param nick Il nickname su cui controllare.
 * @return True se l'history è piena, false altrimenti.
 */
bool is_history_full(nickname_t* nick);

/**
 * @brief Aggiunge un messaggio alla history. Gestisce la concorrenza e libera
 * la memoria quando serve.
 *
 * TODO (opt): non fargli acquisire la lock. In chatty:627 (nel case di
 * POSTTXTALL_OP) viene eseguita subito prima di un'altra operazione che
 * necessita della stessa lock.
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

/**
 * @brief Cerca un nome di file nella history
 *
 * @param nick Il nickname_t in cui cercare il file
 * @param name Il nome del file da cercare
 * @return True se il nome del file è nella history, false altrimenti
 */
bool search_file_history(nickname_t* nick, char* name);


// ============================== htable_t =====================================

/**
 * @struct htable_t
 * @brief Hashtable condivisa che contiene l'insieme dei nickname registrati
 *
 * Questa hashtable usa come chiavi i nickname: se un nickname si trova
 * nell'hashtable allora è registrato. Il valore associato ad ogni chiave è un
 * nickname_t, contenente tutte le informazioni associate ad un nickname.
 *
 * L'hashtable gestisce la concorrenza solo di rimozioni e inserimenti. Le
 * modifiche ai singoli elementi devono essere sincronizzate separatamente.
 */

/**
 * @struct htable
 * @brief Implementazione di htable_t
 * @var struct htable::htable L'hashtable senza sincronizzazione
 * @var struct htable::hist_size La dimensione della history
 * @var struct htable::mutex Mutex interna dell'hashtable per implementare la
 *                           sincronizzazione
 */
typedef struct htable {
	icl_hash_t* htable;
	int hist_size;
	pthread_mutex_t mutex;
} htable_t;


/**
 * @brief Inizializza una nuova hashtable vuota
 * @param nbuckets Il numero di buckets per l'hashtable. Consigliato il doppio
 *                 del numero di elementi atteso.
 * @param history_size La lunghezza della history da allocare agli elementi.
 * @return La nuova hashtable
 */
htable_t* hash_create(int nbuckets, int history_size);

/**
 * @brief Elimina un'hashtable per liberare la memoria
 * @param ht l'hashtable da eliminare
 * @return 0 in caso di successo, <0 in caso di errore
 */
int ts_hash_destroy(htable_t* ht);

/**
 * @brief Cerca una chiave nell'hashtable
 * @param ht L'hashtable in cui cercare la chiave
 * @param key La chiave da cercare
 * @return Il puntatore al valore se la chiave è presente, altrimenti NULL
 */
nickname_t* ts_hash_find(htable_t* ht, char* key);

/**
 * @brief Thread-safe insert. Se la chiave è già presente, non fa nulla.
 *
 * Inserisce la chiave senza nessun dato associato
 * @param ht L'hashtable in cui inserire la chiave
 * @param key La chiave da inserire
 * @return Un puntatore al nuovo elemento. Se l'elemento era già presente, NULL
 */
nickname_t* ts_hash_insert(htable_t* ht, char* key);

/**
 * @brief Thread-safe remove
 *
 * Rimuove una chiave
 * @param ht L'hashtable da cui eliminare la chiave
 * @param key La chiave da eliminare
 * @return true se la chiave era presente, false altrimenti
 */
bool ts_hash_remove(htable_t* ht, char* key);

#endif /* CHATTERBOX_HASH_H_ */
