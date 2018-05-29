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

#define KEY_T char* /**< il tipo delle chiavi dell'hashtable */

/**
 * @struct htable_t
 * @brief Hashtable condivisa che contiene un insieme di stringhe
 *
 * L'hashtable implementa una gestione della concorrenza a lettori-scrittori:
 * i lettori non devono agire in mutua esclusione l'uno rispetto all'altro,
 * ma devono farlo con gli scrittori. Al contrario gli scrittori agiscono in
 * mutua esclusione tra di loro e con i lettori.
 * Viene implementata una politica per evitare la starvation degli scrittori
 */

/**
 * @struct htable
 * @brief Implementazione di htable_t
 * @var struct htable::htable L'hashtable senza sincronizzazione
 * @var struct htable::reader Numero di lettori che stanno operando
 * @var struct htable::writer Numero di scrittori che stanno operando
 * @var struct htable::wait_reader Numero di lettori in attesa
 * @var struct htable::wait_writer Numero di scrittori in attesa
 * @var struct htable::mutex Mutex interna dell'hashtable' per implementare la
 *                           sincronizzazione
 * @var struct htable::cond_read Variabile di condizione interna su cui si bloccano
 *                             i thread lettori se trovano qualche scrittore
 * @var struct htable::cond_read Variabile di condizione interna su cui si bloccano
 *                             i thread scrittori se qualcun altro sta usando la
 *                             hashtable
 */
typedef struct htable {
	icl_hash_t htable;
	int reader, writer, wait_reader, wait_writer;
	pthread_mutex_t mutex;
	pthread_cond_t cond_read, cond_write;
} htable_t;


/**
 * @brief Inizializza una nuova hashtable vuota
 * @return la nuova hashtable
 */
htable_t* ts_hash_create(int nbuckets);

/**
 * @brief Elimina un'hashtable per liberare la memoria
 * @param ht l'hashtable da eliminare
 */
int ts_hash_destroy(htable_t* ht);

/**
 * @brief Thread-safe find
 *
 * Cerca la chiave passata nell'hashtable
 * @param ht L'hashtable in cui cercare la chiave
 * @param key La chiave da cercare
 * @return true se la chiave è presente, false altrimenti
 */
bool ts_hash_find(htable_t* ht, KEY_T key);

/**
 * @brief Thread-safe insert
 *
 * Inserisce la chiave
 * @param ht L'hashtable in cui inserire la chiave
 * @param key La chiave da inserire
 * @return true se la chiave era già presente, false altrimenti
 */
bool ts_hash_insert(htable_t* ht, KEY_T key);

/**
 * @brief Thread-safe delete
 *
 * Elimina una chiave
 * @param ht L'hashtable da cui eliminare la chiave
 * @param key La chiave da eliminare
 * @return true se la chiave era presente, false altrimenti
 */
bool ts_hash_delete(htable_t* ht, KEY_T key);

#endif /* CHATTERBOX_HASH_H_ */
