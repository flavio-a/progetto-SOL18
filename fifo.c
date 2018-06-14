/**
 * @file fifo.c
 * @brief Implementazione di fifo.h
 */

#include "fifo.h"

/**
 * @struct node
 * @brief Nodo di una double linked list
 */
struct node {
	TYPE_T v;
	struct node* next;
	struct node* prev;
};


// ------------------ Funzioni interne ---------------

/**
 * @brief Controlla se la coda è vuota
 * @return True se e solo se la coda è vuota
 */
static inline bool is_empty(fifo_t queue) {
	return queue.head == NULL;
}

/**
 * @brief Implementazione della pop sulla double linked list
 */
static TYPE_T pop(fifo_t* queue) {
	// si aspetta che l'utente abbia già controllato che queue non sia vuota
	node_t* last = queue->tail;
	TYPE_T res = queue->tail->v;
	if (queue->tail == queue->head)
		// se è l'unico elemento rimasto
		queue->tail = queue->head = NULL;
	else {
		queue->tail = queue->tail->prev;
		queue->tail->next = NULL;
	}
	free(last);
	return res;
}

/**
 * @brief Implementazione della push sulla double linked list
 */
static void push(fifo_t* queue, TYPE_T v) {
	node_t* new = malloc(sizeof(node_t));
	new->prev = NULL;
	new->v = v;
	new->next = queue->head;
	if (queue->head == NULL)
		queue->tail = new;
	else
		queue->head->prev = new;
	queue->head = new;
}

// ------- Funzioni esportate --------------
// Documentate in fifo.h

// crea una nuova coda
fifo_t create_fifo() {
	fifo_t res;
	res.head = res.tail = NULL;
	pthread_mutex_init(&(res.mutex), NULL);
	pthread_cond_init(&(res.cond_empty), NULL);
	return res;
}

// svuota la coda
void clear_fifo(fifo_t* q) {
	error_handling_lock(&(q->mutex));
	while (!is_empty(*q))
		// implementazione non ottimale, la pop fa del lavoro inutile
		pop(q);
	error_handling_unlock(&(q->mutex));
	pthread_mutex_destroy(&(q->mutex));
}

// Thread-safe pop
TYPE_T ts_pop(fifo_t* q) {
	TYPE_T n;
	error_handling_lock(&(q->mutex));
	while (is_empty(*q))
		pthread_cond_wait(&(q->cond_empty), &(q->mutex));
	n = pop(q);
	error_handling_unlock(&(q->mutex));
	return n;
}

// Thread-safe push
void ts_push(fifo_t* q, TYPE_T v) {
	error_handling_lock(&(q->mutex));
	push(q, v);
	pthread_cond_signal(&(q->cond_empty));
	error_handling_unlock(&(q->mutex));
}

bool ts_is_empty(fifo_t q){
	bool r;
	error_handling_lock(&(q.mutex));
	r = is_empty(q);
	error_handling_unlock(&(q.mutex));
	return r;
}
