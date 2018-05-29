/**
 * @file fifo.h
 * @brief Libreria per la coda condivisa
 */
#ifndef CHATTERBOX_FIFO_H_
#define CHATTERBOX_FIFO_H_

#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include <message.h>
#include "lock.h"

#define TYPE_T int /**< il tipo degli elementi della coda */

// strutture dati private, ridichiarate qui per necessità
typedef struct node node_t;
typedef node_t* dllist_t; /**< Tipo della double linked list*/

/**
 * @struct fifo
 * @brief Implementazione di fifo_t con una double linked list
 * @var struct fifo::head Testa dalla lista
 * @var struct fifo::tail Coda dalla lista
 * @var struct fifo::mutex Mutex interna della coda per implementare la
 *                         sincronizzazione tra thread
 * @var struct fifo::cond_empty Variabile di condizione interna su cui si bloccano
 *                              i thread che trovano la coda vuota in attesa di
 *                              nuovi elementi
 */
typedef struct fifo {
	dllist_t head, tail;
	pthread_mutex_t mutex;
	pthread_cond_t cond_empty;
} fifo_t;

/**
 * @brief Inizializza una nuova coda vuota
 * @return la nuova Coda
 */
fifo_t create_fifo();

/**
 * @brief Svuota una coda per liberare la memoria
 * @param q la coda da svuotare
 */
void clear_fifo(fifo_t* q);

/**
 * @brief Thread-safe pop
 *
 * Rimuove il primo elemento dalla coda, gestendo la sincronizzazione tra
 * thread
 * @param q La coda da cui estrarre l'elemento
 * @return l'elemento rimosso dalla coda
 */
TYPE_T ts_pop(fifo_t* q);

/**
 * @brief Thread-safe push
 *
 * Inserisce un nuovo elementi in fondo alla coda, gestendo la sincronizzazione
 * tra thread
 * @param q La coda in cui inserire l'elemento
 * @param v l'elemento da inserire
 */
void ts_push(fifo_t* q, TYPE_T v);

/**
 * @brief Thread-safe is_empty
 *
 * Controlla se la coda è vuota, gestendo la sincronizzazione tra thread
 * @param q La coda da controllare
 * @return true se e solo se la coda è vuota
 */
bool ts_is_empty(fifo_t q);

#endif /* CHATTERBOX_FIFO_H_ */
