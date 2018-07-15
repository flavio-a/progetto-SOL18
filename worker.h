/**
 * membox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 * @file worker.h
 * @brief Header per worker.c
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#ifndef WORKER_H_
#define WORKER_H_

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "connections.h"
#include "stats.h"
#include "fifo.h"
#include "ops.h"
#include "hashtable.h"
#include "lock.h"

#define TERMINATION_FD -1
#define FILE_SIZE_FACTOR 1024

/**
 * Struttura che memorizza le statistiche del server, struct statistics
 * è definita in stats.h.
 */
extern statistics chattyStats;
extern pthread_mutex_t stats_mutex;

/**
 * Macro per incrementare le statistiche in modo sicuro
 *
 * @param statName il nome della statistica da incrementare
 */
#define increaseStat(statName) \
	error_handling_lock(&stats_mutex); \
	++chattyStats.statName; \
	error_handling_unlock(&stats_mutex)


/**
 * Coda condivisa che contiene i messaggi
 */
extern fifo_t queue;

/**
 * Array di variabili condivise con il listener, una per ogni vorker
 */
extern int* freefd;
extern char* freefd_ack;

/**
 * Tid del signal handler
 */
extern pthread_t signal_handler;

/**
 * Variabile globale per interrompere i cicli infiniti dei thread
 */
extern bool threads_continue;

/**
 * Hashtable condivisa che contiene i nickname registrati
 */
extern htable_t* nickname_htable;

/**
 * Informazioni sui client connessi
 */
extern int num_connected;
extern char** fd_to_nickname;
extern pthread_mutex_t connected_mutex;

/**
 * Costanti globali lette dal file di configurazione
 */
extern int ThreadsInPool;
extern int MaxHistMsgs;
extern int MaxMsgSize;
extern int MaxFileSize;
extern int MaxConnections;
extern char* DirName;
extern char* StatFileName;
extern char* UnixPath;

/**
 * @brief main di un thread worker, che esegue una operazione alla volta
 *
 * I thread worker si mettono in attesa sulla coda condivisa per delle
 * operazioni da svolgere; non appena ne ricevono una la eseguono, rispondendo
 * al client che l'ha richiesta.
 *
 * @param arg il proprio numero d'indice
 */
void* worker_thread(void* arg);

#endif
