/**
 * @file fifo.h
 * @brief Libreria per la coda condivisa
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

#define TYPE_T char* /**< il tipo degli elementi dell'hashtable */

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




#endif /* CHATTERBOX_HASH_H_ */
