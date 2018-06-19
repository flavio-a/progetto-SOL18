/**
 * @file lock.c
 * @brief Implementazione di lock.h
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#include "lock.h"

void error_handling_lock(pthread_mutex_t* mutex) {
	int attempts = LOCK_MAX_ATTEMPTS;
	while (pthread_mutex_lock(mutex) != 0 && attempts-- > 0) {
		perror("WARNING: error during lock, retrying");
		sleep(LOCK_RETRY_TIME);
	}
	if (attempts == 0) {
		perror("Error during lock, abort\n");
		exit(EXIT_FAILURE);
	}
}

void error_handling_unlock(pthread_mutex_t* mutex) {
	if (pthread_mutex_unlock(mutex) != 0) {
		perror("Error during unlock, abort\n");
		exit(EXIT_FAILURE);
	}
}
