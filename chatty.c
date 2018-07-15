/**
 * membox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 * @file chatty.c
 * @brief File principale del server chatterbox
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "connections.h"
#include "stats.h"
#include "fifo.h"
#include "ops.h"
#include "hashtable.h"
#include "lock.h"
#include "worker.h"

#define NICKNAME_HASH_BUCKETS_N 100000
#define CONFIG_LINE_LENGTH 1024

/**
 * Struttura che memorizza le statistiche del server, struct statistics
 * è definita in stats.h.
 */
statistics chattyStats = { 0,0,0,0,0,0,0 };
pthread_mutex_t stats_mutex;

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
fifo_t queue;

/**
 * Array di variabili condivise con il listener, una per ogni vorker
 */
int* freefd;
char* freefd_ack;

/**
 * Tid del signal handler
 */
pthread_t signal_handler;

/**
 * Variabile globale per interrompere i cicli infiniti dei thread
 */
bool threads_continue = true;

/**
 * Hashtable condivisa che contiene i nickname registrati
 */
htable_t* nickname_htable;

/**
 * Informazioni sui client connessi
 */
int num_connected = 0;
char** fd_to_nickname;
pthread_mutex_t connected_mutex;

/**
 * Costanti globali lette dal file di configurazione
 */
int ThreadsInPool;
int MaxHistMsgs;
int MaxMsgSize;
int MaxFileSize;
int MaxConnections;
char* DirName;
char* StatFileName;
char* UnixPath;

/**
 * @brief Funzione che spiega l'utilizzo del server
 * @param progname il nome del file eseguibile (argv[0])
 */
void usage(const char *progname) {
	fprintf(stderr, "Il server va lanciato con il seguente comando:\n");
	fprintf(stderr, "  %s -f conffile\n", progname);
}


/**
 * @brief main del thread che si occupa della gestione dei segnali
 *
 * Non fa niente finché il processo non riceve un segnale. In quel caso questo
 * thread si attiva e lo gestisce
 *
 * In caso di segnale di terminazione, questa funzione ritorna per restituire
 * il controllo al main, che a sua volta termina dopo aver eseguito un po' di
 * cleanup.
 *
 * @param handled_signals L'insieme dei segnali da gestire, visto che il main
 *                        deve già crearlo per mascherarli in tutti i thread
 */
void signal_handler_thread(sigset_t* handled_signals) {
	const int statsfd = MaxConnections + ThreadsInPool + 1;
	const int list_pipefd = MaxConnections + ThreadsInPool + 2;
	char listener_ack_val = 1;
	int sig_received;

	while(threads_continue) {
		if (sigwait(handled_signals, &sig_received) < 0) {
			perror("sigwait");
			exit(EXIT_FAILURE);
		}
		if (sig_received == SIGUSR1) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale SIGUSR1\n");
				#if defined VERBOSE
					fprintf(stderr, "Elenco utenti connessi:\n");
					int i;
					icl_entry_t* j;
					char* key;
					nickname_t* val;
					icl_hash_foreach(nickname_htable->htable, i, j, key, val) {
						if (val->fd != 0) {
							fprintf(stderr, "  %s\n", key);
						}
	 				}
				#endif
			#endif
			// gestire il segnale
			int filefd = open(StatFileName, O_WRONLY | O_CREAT | O_APPEND, 0744);
			if (filefd < 0
				|| dup2(filefd, statsfd) < 0) {
				perror("aprendo il file delle statistiche");
			}
			else {
				close(filefd);
				if (dprintf(statsfd, "%ld - %d %d %ld %ld %ld %ld %ld\n",
			                 time(NULL),
							 nickname_htable->htable->nentries,
							 num_connected,
							 chattyStats.ndelivered,
							 chattyStats.nnotdelivered,
							 chattyStats.nfiledelivered,
							 chattyStats.nfilenotdelivered,
							 chattyStats.nerrors
						     ) < 0) {
					perror("scrivendo le statistiche");
				}
				close(statsfd);
			}
		}
		else if (sig_received == SIGUSR2) {
			// Deve mandare un ack al listener tramite la pipe
			while (write(list_pipefd, &listener_ack_val, 1) < 0) {
				// Questa write non dovrebbe avere motivo di fallire
				// Se fallisce semplicemente riprovo a mandare l'ack
				perror("write, mandando ack al listener, riprovo");
			}
		}
		else if (sig_received == SIGINT || sig_received == SIGTERM || sig_received == SIGQUIT) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale di interruzione, chiudo su tutto per bene\n");
			#endif
			threads_continue = false;
			// Sblocca il listener scrivendogli sulla pipe
			while (write(list_pipefd, &listener_ack_val, 1) < 0) {
				perror("write, mandando ack al listener, riprovo");
			}
			// Sblocca i worker con un TERMINATION_FD
			for (unsigned int i = 0; i < ThreadsInPool; ++i) {
				ts_push(&queue, TERMINATION_FD);
			}
			break;
		}
	}

	close(list_pipefd);
	return;
}


/**
 * @brief main del thread listener, che gestisce le connessioni con i client
 *
 * Gestisce sia le richieste di nuove connessioni, sia i messaggi inviati dai
 * client già connessi.
 *
 * @param arg Nulla (si può passare NULL)
 */
void* listener_thread(void* arg) {
	// Non c'è bisogno di leggerlo, deve esere 3 per forza
	const int ssfd = 3;
	const int pipefd = 4;
	char ack_buf[ThreadsInPool];
	// Indice del massimo fd atteso nella select
	int fdnum = pipefd;

	// Preparazione iniziale del fd_set
	fd_set set, rset;
	FD_ZERO(&set);
	FD_SET(ssfd, &set);
	FD_SET(pipefd, &set);

	// Ciclo di esecuzione
	while (threads_continue) {
		rset = set;
		// Usa la SC select per ascoltare contemporaneamente su molti socket
		#if defined DEBUG && defined VERBOSE
			fprintf(stderr, "Inizia la select\n");
		#endif
		if (select(fdnum + 1, &rset, NULL, NULL, NULL) < 0) {
			perror("select del listener");
		}
		else {
			// Select terminata correttamente: controlla quale fd è pronto
			#if defined DEBUG && defined VERBOSE
                fprintf(stderr, "Ricevuto qualcosa dalla select\n");
            #endif
			for (int fd = 0; fd <= fdnum; ++fd) {
				if (FD_ISSET(fd, &rset)) {
					if (fd == pipefd) {
						// Cancella tutti gli ack del signal handler. Visto che
						// gli ack dovrebbero arrivare solo dai worker,
						// dovrebbero essere al massimo ThreadsInPool
						while (read(pipefd, ack_buf, ThreadsInPool) < 0) {
							perror("errore leggendo gli ack, riprovo");
							// In caso di errore semplicemente riprova
						}
						// Controlla quali ack sono a 1 (eventualmente nessuno,
						// in caso di segnali spurii)
						for (int i = 0; i < ThreadsInPool; ++i) {
							if (freefd_ack[i] == 1) {
								// Aggiunge freefd[i] alla bitmap su cui esegue
								// la select
								FD_SET(freefd[i], &set);
								// Non c'è bisogno di aggiornare fdnum perché un
								// fd arrivato da un worker è stato accettato dal
								// listener, quindi ha già modificato fdnum
								#if defined DEBUG && defined VERBOSE
									fprintf(stderr, "Ricevuto fd %d da un worker\n", freefd[i]);
								#endif
								freefd_ack[i] = 0;
							}
						}
					}
					else if (fd == ssfd) {
						// Richiesta di nuova connessione
						int newfd = accept(ssfd, NULL, 0);
						#ifdef DEBUG
							fprintf(stderr, "Richiesta di nuova connessione: %d\n", newfd);
						#endif
						// Accetta al massimo MaxConnections dai client
						if (newfd < MaxConnections) {
							FD_SET(newfd, &set);
							if (newfd > fdnum) {
								fdnum = newfd;
							}
						}
						else {
							increaseStat(nerrors);
							close(newfd);
						}
					}
					else {
						// Richiesta su una connessione già aperta
						#ifdef DEBUG
							fprintf(stderr, "Richiesta su fd %d\n", fd);
						#endif
						ts_push(&queue, fd);
						FD_CLR(fd, &set);
					}
				}
			}
		}
	}

	close(pipefd);
	return NULL;
}


/**
 * @function main
 * @brief Punto di ingresso del server
 *
 * Si occupa di leggere il file di configurazione, inizializzare la coda
 * condivisa e il socket e avviare gli altri thread. Una volta fatto questo,
 * esegue la funzione signal_handler_thread per gestire i segnali. Se questa
 * ritorna aspetta la fine degli altri thread, poi cancella le strutture dati
 * condivise per liberare memoria.
 */
int main(int argc, char *argv[]) {
	// Un po' di assert iniziali
	assert(TERMINATION_FD < 0);
	assert(NULL == 0);

	// Lettura dell'argomento
	if (argc <= 1 || strncmp(argv[1], "-f", 2) != 0) {
		usage(argv[0]);
		return -1;
	}
	char* conf_path;
	if (argc == 2) {
		// se c'è un solo parametro, assume che contenga il nome del file attaccato a -f
		conf_path = argv[1] + 2;
	}
	else {
		conf_path = argv[2];
	}

	FILE* conf_file;
	if ((conf_file = fopen(conf_path, "r")) == NULL) {
		perror(conf_path);
		exit(EXIT_FAILURE);
	}

	// Parsing del file di configurazione
	char* line = malloc(CONFIG_LINE_LENGTH * sizeof(char));
	while (true) { // Uscita dal break interno
		if (fgets(line, CONFIG_LINE_LENGTH, conf_file) == NULL) {
			if (feof(conf_file)) {
				break;
			}
			// Linea di configurazione troppo lunga
			perror("leggendo il file di configurazione, linea troppo lunga");
			exit(-1);
		}
		// Controlla i commenti
		if (line[0] != '#') {
			char* paramName = strtok(line, " \n	");
			char* equalSign = strtok(NULL, " \n	");
			char* paramValue = strtok(NULL, " \n	");
			// Controlla le linee che non sono parametri
			if (paramName != NULL && paramName[0] != '\n' && equalSign[0] == '=' && equalSign[1] == '\0') {
				if (strncmp(paramName, "ThreadsInPool", strlen("ThreadsInPool") + 1) == 0) {
					ThreadsInPool = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto ThreadsInPool: %d\n", ThreadsInPool);
					#endif
				}
				else if (strncmp(paramName, "MaxHistMsgs", strlen("MaxHistMsgs") + 1) == 0) {
					MaxHistMsgs = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxHistMsgs: %d\n", MaxHistMsgs);
					#endif
				}
				else if (strncmp(paramName, "MaxMsgSize", strlen("MaxMsgSize") + 1) == 0) {
					MaxMsgSize = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxMsgSize: %d\n", MaxMsgSize);
					#endif
				}
				else if (strncmp(paramName, "MaxFileSize", strlen("MaxFileSize") + 1) == 0) {
					MaxFileSize = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxFileSize: %d\n", MaxFileSize);
					#endif
				}
				else if (strncmp(paramName, "MaxConnections", strlen("MaxConnections") + 1) == 0) {
					MaxConnections = strtol(paramValue, NULL, 10);
					MaxConnections += 5; // Serve solo aumentata
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxConnections: %d\n", MaxConnections);
					#endif
				}
				else if (strncmp(paramName, "UnixPath", strlen("UnixPath") + 1) == 0) {
					UnixPath = malloc((strlen(paramValue) + 1) * sizeof(char));
					strncpy(UnixPath, paramValue, strlen(paramValue) + 1);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto UnixPath: %s\n", UnixPath);
					#endif
				}
				else if (strncmp(paramName, "DirName", strlen("DirName") + 1) == 0) {
					DirName = malloc((strlen(paramValue) + 2) * sizeof(char));
					strncpy(DirName, paramValue, strlen(paramValue));
					// Serve solo con lo / finale
					DirName[strlen(paramValue)] = '/';
					DirName[strlen(paramValue) + 1] = '\0';
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto DirName: %s\n", DirName);
					#endif
				}
				else if (strncmp(paramName, "StatFileName", strlen("StatFileName") + 1) == 0) {
					StatFileName = malloc((strlen(paramValue) + 1) * sizeof(char));
					strncpy(StatFileName, paramValue, strlen(paramValue) + 1);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto StatFileName: %s\n", StatFileName);
					#endif
				}
			}
		}
	}
	free(line);

	fclose(conf_file);

	// Ignora SIGPIPE per tutto il processo
	struct sigaction s;
	memset(&s, 0, sizeof(s));
	s.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &s, NULL) < 0) {
		perror("sigaction");
		return -1;
	}

	// Crea la bitmap per mascherare i segnali comuni a tutti i thread, che la
	// erediteranno alla creazione.
	sigset_t signalmask;
	// Questo if dovrebbe eseguire perror appena dopo la chiamata errata per la
	// lazy evaluation dell'OR
	if (sigemptyset(&signalmask) < 0
		|| sigaddset(&signalmask, SIGUSR1) < 0
		|| sigaddset(&signalmask, SIGUSR2) < 0
		|| sigaddset(&signalmask, SIGINT) < 0
		|| sigaddset(&signalmask, SIGTERM) < 0
		|| sigaddset(&signalmask, SIGQUIT) < 0
	) {
		perror("creando la bitmap per mascherare i segnali nel main");
		exit(EXIT_FAILURE);
	}
	if (pthread_sigmask(SIG_SETMASK, &signalmask, NULL) == -1) {
		perror("pthread_sigmask nel main");
		exit(EXIT_FAILURE);
	}

	// Crea le strutture condivise
	queue = create_fifo();
	nickname_htable = hash_create(NICKNAME_HASH_BUCKETS_N, MaxHistMsgs);
	int socketfd = createSocket(UnixPath);
	if (socketfd != 3) {
		if (dup2(socketfd, 3) < 0) {
			perror("errore spostando il socket su fd 3");
			exit(EXIT_FAILURE);
		}
		else {
			close(socketfd);
		}
	}
	int pipefd[2];
	if (pipe(pipefd) < 0) {
		perror("creando la pipe");
		exit(EXIT_FAILURE);
	}
	if (pipefd[0] != 4) {
		if (dup2(pipefd[0], 4) < 0) {
			perror("errore spostando il la pipe su fd 4");
			exit(EXIT_FAILURE);
		}
		else {
			close(pipefd[0]);
		}
	}
	if (pipefd[1] != 2 + MaxConnections + ThreadsInPool) {
		if (dup2(pipefd[1], 2 + MaxConnections + ThreadsInPool) < 0) {
			perror("errore spostando la pipe su fd 2 + roba");
			exit(EXIT_FAILURE);
		}
		else {
			close(pipefd[1]);
		}
	}
	pthread_t listener;
	pthread_t pool[ThreadsInPool];
	if ((freefd = malloc(ThreadsInPool * sizeof(int))) == NULL
		|| (freefd_ack = calloc(ThreadsInPool, sizeof(char))) == NULL
		|| (fd_to_nickname = calloc(MaxConnections, sizeof(char*))) == NULL
		) {
		perror("out of memory");
		exit(EXIT_FAILURE);
	}
	pthread_mutex_init(&connected_mutex, NULL);
	pthread_mutex_init(&stats_mutex, NULL);
	signal_handler = pthread_self();
	// Crea i vari thread
	pthread_create(&listener, NULL, &listener_thread, NULL);
	for (unsigned int i = 0; i < ThreadsInPool; ++i) {
		// ricicla lo spazio di freefd per passare ai worker il loro numero
		freefd[i] = i;
		pthread_create(pool + i, NULL, &worker_thread, freefd + i);
	}
	// Diventa il thread che gestisce i segnali
	signal_handler_thread(&signalmask);
	// Se signal_handler_thread ritorna vuol dire che deve aspettare gli altri
	// thread, eseguire i cleanup finali e poi terminare
	// In teoria queste chiamate non possono ritornare errore (gli errori
	// segnati in man pthread_join non possono verificarsi in questo caso). E
	// poi che gestione dell'errore faccio, termino il processo?
	pthread_join(listener, NULL);
	for (unsigned int i = 0; i < ThreadsInPool; ++i) {
		pthread_join(pool[i], NULL);
	}

	// Elimina il socket
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Unlink del socket\n");
	#endif
	unlink(UnixPath);
	// Libera la memoria che ha occupato all'inizio del programma
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Cancello la coda condivisa\n");
	#endif
	clear_fifo(&queue);
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Libero gli array di comunicazione listener-worker\n");
	#endif
	free(freefd);
	free(freefd_ack);
	// libera tutti i valori inizializzati di fd_to_nickname
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Svuoto fd_to_nickname\n");
	#endif
	for (int i = 0; i < MaxConnections; ++i) {
		if (fd_to_nickname[i] != NULL) {
			free(fd_to_nickname[i]);
		}
	}
	free(fd_to_nickname);
	// Non ci sono altri thread oltre a main, quindi nessuno ha il lock
	pthread_mutex_destroy(&connected_mutex);
	pthread_mutex_destroy(&stats_mutex);
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Elimino l'hashtable\n");
	#endif
	ts_hash_destroy(nickname_htable);

	return 0;
}
