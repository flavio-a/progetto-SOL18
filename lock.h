/**
 * @file lock.h
 * @brief Libreria per lock e unlock un po' più robuste condivisa
 */
#ifndef CHATTERBOX_LOCK_H_
#define CHATTERBOX_LOCK_H_

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#define LOCK_RETRY_TIME 1 /**< Tempo di attesa in secondi dopo un tentativo di
                               lock fallito */
#define LOCK_MAX_ATTEMPTS 10 /**< Numero di tentativi di lock prima di terminare
                                 il programma */

/**
* @brief Tenta di acquisire un lock con una certa tolleranza agli errori
*
* In caso di fallimento, il lock viene riprovato ad intervalli di LOCK_RETRY_TIME
* fino ad un massimo di LOCK_MAX_ATTEMPTS volte. Dopo questi tentativi, il
* programma viene terminato con un errore
* @param mutex La mutex da acquisire
*/
void error_handling_lock(pthread_mutex_t* mutex);

/**
* @brief Tenta di rilasciare un lock
*
* In caso di fallimento, il programma viene subito terminato con un errore
* dato che il fallimento di una lock release è un errore molto grave e può
* portare ad una lock in stato inconsistente
* @param mutex La mutex da rilasciare
*/
void error_handling_unlock(pthread_mutex_t* mutex);

#endif /* CHATTERBOX_LOCK_H_ */
