/*
 * membox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 */
/**
 * @file chatty.c
 * @brief File principale del server chatterbox
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/select.h>

#include "connections.h"
#include "stats.h"
#include "fifo.h"
#include "ops.h"
#include "hashtable.h"
#include "lock.h"

#define NICKNAME_HASH_BUCKETS_N 100000
#define INITIAL_CONNECTED_SIZE 200

/**
 * Struttura che memorizza le statistiche del server, struct statistics
 * è definita in stats.h.
 */
statistics chattyStats = { 0,0,0,0,0,0,0 };

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
 * Flag per segnalare al listener che ha ricevuto un SIGUSR2
 */
volatile sig_atomic_t received_sigusr2 = false;

/**
 * Tid del listener
 */
pthread_t listener;

/**
 * Hashtable condivisa che contiene i nickname registrati
 */
htable_t* nickname_htable;

/**
 * Informazioni sui client connessi
 */
int num_connected = 0;
char** fd_to_nickname;
int fdnum;
pthread_mutex_t connected_mutex;

/**
 * @brief Funzione che spiega l'utilizzo del server
 * @param progname il nome del file eseguibile (argc[0])
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
 * cleanup
 */
void signal_handler_thread() {
	// crea il set dei segnali da attendere con la sigwait
	sigset_t handled_signals;
	int sig_received;
	if (sigemptyset(&handled_signals) < 0
		|| sigaddset(&handled_signals, SIGUSR1) < 0
		|| sigaddset(&handled_signals, SIGINT) < 0
		|| sigaddset(&handled_signals, SIGTERM) < 0
		|| sigaddset(&handled_signals, SIGQUIT) < 0
	) {
		perror("creando la bitmap per aspettare i segnali nel signal handler");
		exit(EXIT_FAILURE);
	}

	while(true) { // uscita dal ciclo con il break interno
		if (sigwait(&handled_signals, &sig_received) < 0) {
			perror("sigwait");
			exit(EXIT_FAILURE);
		}
		if (sig_received == SIGUSR1) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale SIGUSR1\n");
			#endif
			// temporaneo, per il debug
			int i;
			icl_entry_t* j;
			char* key;
			nickname_t* val;
			icl_hash_foreach(nickname_htable->htable, i, j, key, val) {
				if (val->fd != 0) {
					fprintf(stderr, "Connesso %s\n", key);
				}
			}
			// gestire il segnale
		}
		if (sig_received == SIGINT || sig_received == SIGTERM || sig_received == SIGQUIT) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale di interruzione, chiudo su tutto per bene\n");
			#endif
			// gestire il segnale
			break;
		}
	}

	return;
}

/**
 * @brief Handler per SIGUSR2, eseguito dal thread listener
 *
 * Setta semplicemente un flag. Ritorna
 */
static void siguser2_handler(int signum) {
	received_sigusr2 = true;
}

/**
 * @brief main del thread listener, che gestisce le connessioni con i client
 *
 * Gestisce sia le richieste di nuove connessioni, sia i messaggi inviati dai
 * client già connessi
 *
 * @param arg array di due interi, rispettivamente il fd del socket e il numero
 *			di worker
 */
void* listener_thread(void* arg) {
	// Salva gli input
	int ssfd = ((int*)arg)[0];
	int worker_num = ((int*)arg)[1];

	// Smaschera SIGUSR2
	sigset_t sigset;
	if (sigemptyset(&sigset) < 0 || sigaddset(&sigset, SIGUSR2) < 0) {
		perror("creando la bitmap per smascherare SIGUSR2 nel listener");
		exit(EXIT_FAILURE);
	}
	if (pthread_sigmask(SIG_UNBLOCK, &sigset, NULL) < 0) {
		perror("pthread_sigmask nel listener");
		exit(EXIT_FAILURE);
	}
	// Installa l'handler
	struct sigaction sigusr2_action;
	sigusr2_action.sa_handler = &siguser2_handler;
	if (sigaction(SIGUSR2, &sigusr2_action, NULL) < 0) {
		perror("installando l'handler per SIGUSR2");
		exit(EXIT_FAILURE);
	}
	// Preparazione iniziale del fd_set
	fd_set set, rset;
	FD_ZERO(&set);
	FD_SET(ssfd, &set);

	// Ciclo di esecuzione
	while (true) {
		rset = set;
		// Usa la SC select per ascoltare contemporaneamente su molti socket
		if (select(fdnum + 1, &rset, NULL, NULL, NULL) < 0) {
			// Se ho ricevuto un segnale SIGUSR2 controllo se uno dei worker ha
			// liberato un filedescriptor
			if (errno == EINTR && received_sigusr2) {
				received_sigusr2 = false;
				// Controlla quali ack sono a 1 (eventualmente nessuno, in caso
				// di segnali spurii)
				for (int i = 0; i < worker_num; ++i) {
					// pthread_mutex_lock(freefd_lock + i);
					if (freefd_ack[i] == 1) {
						// Aggiunge freefd[i] alla bitmap su cui esegue la select
						FD_SET(freefd[i], &set);
						// Non c'è bisogno di aggiornare fd_num perché un fd
						// arrivato da un worker è stato accettato da listener,
						// quindi ha già modificato fd_num
						freefd_ack[i] = 0;
						#ifdef DEBUG
							fprintf(stderr, "Ricevuto un fd da un worker\n");
						#endif
					}
					// pthread_mutex_unlock(freefd_lock + i);
				}
			}
			else
				perror("select del listener");
		}
		else { // select terminata correttamente
			for (int fd = 0; fd <= fdnum; ++fd) {
				if (FD_ISSET(fd, &rset)) {
					if (fd == ssfd) {
						// richiesta di nuova connessione
						int newfd = accept(ssfd, NULL, 0);
						#ifdef DEBUG
							fprintf(stderr, "Richiesta di nuova connessione: %d\n", newfd);
						#endif
						FD_SET(newfd, &set);
						if (newfd > fdnum) {
							error_handling_lock(&connected_mutex);
							if (newfd > INITIAL_CONNECTED_SIZE) {
								fd_to_nickname = realloc(fd_to_nickname, (newfd + 1) * sizeof(char*));
								for (int i = fdnum; i < newfd; ++i) {
									fd_to_nickname[i] = NULL;
								}
							}
							fdnum = newfd;
							error_handling_unlock(&connected_mutex);
						}
					}
					else {
						// richiesta su una connessione già aperta
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

	return NULL;
}



/**
 * @brief Modifica le strutture dati necessarie alla disconnessione di un client
 * dal fd passato. Se il fd passato non ha associato nessun client, viene
 * solamente chiuso il fd.
 *
 * @param fd
 */
void disconnectClient(int fd) {
	close(fd);
	if (fd_to_nickname[fd] == NULL)
		return;
	#ifdef DEBUG
		fprintf(stderr, "Un client si è disconnesso (fd %d, nick \"%s\") :c\n", fd, fd_to_nickname[fd]);
	#endif
	nickname_t* client = ts_hash_find(nickname_htable, fd_to_nickname[fd]);
	error_handling_lock(&(client->mutex));
	client->fd = 0;
	error_handling_unlock(&(client->mutex));
	error_handling_lock(&connected_mutex);
	--num_connected;
	free(fd_to_nickname[fd]);
	fd_to_nickname[fd] = NULL;
	error_handling_unlock(&connected_mutex);
}

/**
 * @brief TODO
 *
 * Se nick_data == NULL, lo estrae da solo dall'hashtable. Si aspetta che sia
 * già stata acquisita la lock connected_mutex.
 */
void connectClient(char* nick, int fd, nickname_t* nick_data) {
	++num_connected;
	if (nick_data == NULL)
		nick_data = ts_hash_find(nickname_htable, nick);
	nick_data->fd = fd;
	fd_to_nickname[fd] = malloc(strlen(nick) * sizeof(char*));
	strncpy(fd_to_nickname[fd], nick, strlen(nick));
}

/**
 * @brief Crea un messaggio contenente l'elenco dei nickname connessi da usare
 * come risposta per un client. Si aspetta che sia già stata acquisita la lock
 * connected_mutex.
 *
 * @param msg Puntatore al messaggio che verrà poi spedito come risposta.
 */
void responseConnectedList(message_t* msg) {
	int p = 0;
	char* conn_list = malloc(num_connected * sizeof(char) * (MAX_NAME_LENGTH + 1));
	for (int i = 0; i < fdnum + 1; ++i) {
		if (fd_to_nickname[i] != NULL) {
			strncpy(conn_list + p * (MAX_NAME_LENGTH + 1), fd_to_nickname[i], MAX_NAME_LENGTH + 1);
			++p;
		}
	}
	#ifdef DEBUG
		fprintf(stderr, "p: %d, num_connected: %d\n", p, num_connected);
		assert(p == num_connected);
	#endif
	setHeader(&(msg->hdr), OP_OK, "");
	setData(&(msg->data), "", conn_list, num_connected * (MAX_NAME_LENGTH + 1));
}

/**
 * @brief main di un thread worker, che esegue una operazione alla volta
 *
 * I thread worker si mettono in attesa sulla coda condivisa per delle operazioni
 * da svolgere; non appena ne ricevono una la eseguono, rispondendo al client
 * che l'ha richiesta
 *
 * @param arg il proprio numero d'indice
 */
void* worker_thread(void* arg) {
	int workerNumber = *(int*)arg;

	while(true) {
		int localfd = ts_pop(&queue);
		// #ifdef DEBUG
		// 	fprintf(stderr, "%d: Inizio lavoro su fd %d\n", workerNumber, localfd);
		// #endif
		message_t msg;
		bool fdclose = false;
		// Le comunicazioni iniziano sempre con un messaggio
		int readRes = readMsg(localfd, &msg);
		if (readRes < 0) {
			if (errno == ECONNRESET) {
				#ifdef DEBUG
					fprintf(stderr, "%d: un client è crashato (fd %d)\n", workerNumber, localfd);
				#endif
				disconnectClient(localfd);
				fdclose = true;
			}
			else {
				perror("leggendo un messaggio");
			}
		}
		else if (readRes == 0) {
			// Client disconnesso
			disconnectClient(localfd);
			fdclose = true;
		}
		else {
			// Messaggio da inviare in risposta al client
			message_t response;
			nickname_t* nick;
			// Quando scrive qualcosa deve sempre controllare se il descrittore
			// è stato chiuso (ritorna -1, errno == EPIPE), in tal caso aggiornare
			// i nickname connessi di conseguenza
			switch (msg.hdr.op) {
				case REGISTER_OP:
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta REGISTER_OP\n", workerNumber);
					#endif
					if ((nick = ts_hash_insert(nickname_htable, msg.hdr.sender)) == NULL) {
						#ifdef DEBUG
							fprintf(stderr, "%d: Nickname %s già esistente!\n", workerNumber, msg.hdr.sender);
						#endif
						setHeader(&(response.hdr), OP_NICK_ALREADY, "");
						setData(&(response.data), "", NULL, 0);
					}
					else {
						#ifdef DEBUG
							fprintf(stderr, "%d: Registrato il nickname \"%s\"\n", workerNumber, msg.hdr.sender);
						#endif
						pthread_mutex_lock(&connected_mutex);
						connectClient(msg.hdr.sender, localfd, nick);
						responseConnectedList(&response);
						pthread_mutex_unlock(&connected_mutex);
					}
					break;
				case CONNECT_OP:
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta CONNECT_OP\n", workerNumber);
					#endif
					if ((nick = ts_hash_find(nickname_htable, msg.hdr.sender)) != NULL) {
						pthread_mutex_lock(&connected_mutex);
						connectClient(msg.hdr.sender, localfd, nick);
						responseConnectedList(&response);
						pthread_mutex_unlock(&connected_mutex);
					}
					else {
						#ifdef DEBUG
							fprintf(stderr, "%d: Richiesta di connessione di un nickname inesistente\n", workerNumber);
						#endif
						setHeader(&(response.hdr), OP_NICK_UNKNOWN, "");
						setData(&(response.data), "", NULL, 0);
					}
					break;
				case USRLIST_OP:
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta USRLIST_OP\n", workerNumber);
					#endif
					pthread_mutex_lock(&connected_mutex);
					responseConnectedList(&response);
					pthread_mutex_unlock(&connected_mutex);
					break;
				default:
					break;
			}
			#ifdef DEBUG
				fprintf(stderr, "%d: Invio risposta al client\n", workerNumber);
			#endif
			if (sendRequest(localfd, &response) < 0) {
				if (errno == EPIPE) {
					// Client disconnesso
					disconnectClient(localfd);
					fdclose = true;
				}
				else {
					perror("inviando un messaggio");
				}
			}
			if (response.data.buf != NULL) {
				free(response.data.buf);
			}
		}
		// Finita la richiesta segnala al listener che il fd è di nuovo libero
		// se non ha chiuso la connessione
		if (!fdclose) {
			while(freefd_ack[workerNumber] == 1) {
				pthread_kill(listener, SIGUSR2);
				sched_yield();
			}
			freefd[workerNumber] = localfd;
			freefd_ack[workerNumber] = 1;
			pthread_kill(listener, SIGUSR2);
			#ifdef DEBUG
				fprintf(stderr, "%d: Restituito l'fd al listener\n", workerNumber);
			#endif
		}
	}

	return NULL;
}

/**
 * @function main
 * @brief punto di ingresso del server
 *
 * Si occupa di leggere il file di configurazione, inizializzare la coda
 * condivisa e il oscket e avviare gli altri thread. Una volta fatto questo,
 * esegue la funzione signal_handler_thread per gestire i segnali
 */
int main(int argc, char *argv[]) {
	// Lettura dell'argomento
	if (argc <= 1 || strncmp(argv[1], "-f", 2) != 0) {
		usage(argv[0]);
		return -1;
	}
	char* conf_path;
	if (argc == 2)
		conf_path = argv[1] + 2; // se c'è un solo parametro, assume che il
								 // contenga il nome del file attaccato a -f
	else
		conf_path = argv[2];

	FILE* conf_file;
	if ((conf_file = fopen(conf_path, "r")) == NULL) {
		perror(conf_path);
		exit(EXIT_FAILURE);
	}

	// Parsing del file di configurazione
	// TODO: cercare una libreria che lo faccia per me
	int ThreadsInPool = 8;
	int MaxHistMsgs = 16;
	char* UnixPath = "/tmp/chatty_socket";

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
	// Il listener, che non deve mascherare SIGUSR2, si occuperà di smascherarlo
	sigset_t worker_signalmask;
	// Questo if dovrebbe eseguire perror appena dopo la chiamata errata per la
	// lazy evaluation dell'OR
	if (sigemptyset(&worker_signalmask) < 0
		|| sigaddset(&worker_signalmask, SIGUSR1) < 0
		|| sigaddset(&worker_signalmask, SIGUSR2) < 0
		|| sigaddset(&worker_signalmask, SIGINT) < 0
		|| sigaddset(&worker_signalmask, SIGTERM) < 0
		|| sigaddset(&worker_signalmask, SIGQUIT) < 0
	) {
		perror("creando la bitmap per mascherare i segnali nel main");
		exit(EXIT_FAILURE);
	}
	if (pthread_sigmask(SIG_SETMASK, &worker_signalmask, NULL) == -1) {
		perror("pthread_sigmask nel main");
		exit(EXIT_FAILURE);
	}

	// Crea le strutture condivise
	int listener_params[2];
	queue = create_fifo();
	nickname_htable = hash_create(NICKNAME_HASH_BUCKETS_N, MaxHistMsgs);
	listener_params[0] = createSocket(UnixPath);
	pthread_t pool[ThreadsInPool];
	freefd = malloc(ThreadsInPool * sizeof(int));
	freefd_ack = calloc(ThreadsInPool, sizeof(char));
	pthread_mutex_init(&connected_mutex, NULL);
	fdnum = listener_params[0];
	assert(INITIAL_CONNECTED_SIZE >= fdnum);
	fd_to_nickname = calloc(INITIAL_CONNECTED_SIZE + 1 , sizeof(char*));
	// Crea i vari thread
	listener_params[1] = ThreadsInPool;
	pthread_create(&listener, NULL, &listener_thread, (void*)listener_params);
	for (unsigned int i = 0; i < ThreadsInPool; ++i) {
		// ricicla lo spazio di freefd per passare ai worker il loro numero
		freefd[i] = i;
		pthread_create(pool + i, NULL, &worker_thread, (void*)(freefd + i));
	}
	// Diventa il thread che gestisce i segnali
	signal_handler_thread();
	// Se signal_handler_thread ritorna vuol dire che deve eseguire i cleanup
	// finali e poi terminare

	// Elimina il socket
	unlink(UnixPath);
	// Libera la memoria che ha occupato all'inizio del programma
	clear_fifo(&queue);
	free(freefd);
	free(freefd_ack);
	// libera tutti i valori inizializzati di fd_to_nickname
	for (int i = 0; i < fdnum; ++i) {
		if (fd_to_nickname[i] != NULL) {
			free(fd_to_nickname[i]);
		}
	}
	free(fd_to_nickname);
	ts_hash_destroy(nickname_htable);
	pthread_mutex_lock(&connected_mutex);
	pthread_mutex_unlock(&connected_mutex);
	pthread_mutex_destroy(&connected_mutex);

	return 0;
}
