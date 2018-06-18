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
#include "nickname.h"
#include "lock.h"

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
 *
 * Questa funzione non è sincronizzata.
 *
 * @param ht L'hashtable in cui cercare la chiave
 * @param key La chiave da cercare
 * @return Il puntatore al valore se la chiave è presente, altrimenti NULL
 */
nickname_t* hash_find(htable_t* ht, char* key);

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
