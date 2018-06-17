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
#define TERMINATION_FD -1

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
int fdnum;
pthread_mutex_t connected_mutex;

/**
 * Costanti globali lette dal file di configurazione
 */
int ThreadsInPool;
int MaxMsgSize;
int MaxFileSize;
int MaxConnections;


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

	while(threads_continue) {
		if (sigwait(&handled_signals, &sig_received) < 0) {
			perror("sigwait");
			exit(EXIT_FAILURE);
		}
		if (sig_received == SIGUSR1) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale SIGUSR1\n");
			#endif
			// temporaneo, per il debug
			#ifdef DEBUG
				int i;
				icl_entry_t* j;
				char* key;
				nickname_t* val;
				icl_hash_foreach(nickname_htable->htable, i, j, key, val) {
					if (val->fd != 0) {
						fprintf(stderr, "Connesso %s\n", key);
					}
				}
			#endif
			// gestire il segnale
		}
		if (sig_received == SIGINT || sig_received == SIGTERM || sig_received == SIGQUIT) {
			#ifdef DEBUG
				fprintf(stderr, "Ricevuto segnale di interruzione, chiudo su tutto per bene\n");
			#endif
			threads_continue = false;
			// Sblocca il listener con un SIGUSR2
			// Si potrebbe usare un segnale diverso, ma è sbatti per quasi nessun guadagno
			pthread_kill(listener, SIGUSR2);
			// Sblocca i worker con un TERMINATION_FD
			for (unsigned int i = 0; i < ThreadsInPool; ++i) {
				ts_push(&queue, TERMINATION_FD);
			}
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
 * @param arg Nulla (si può passare NULL)
 */
void* listener_thread(void* arg) {
	// Salva il fd del socket
	int ssfd = fdnum;

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
	// Installa l'handler per SIGUSR2
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
	while (threads_continue) {
		rset = set;
		// Usa la SC select per ascoltare contemporaneamente su molti socket
		if (select(fdnum + 1, &rset, NULL, NULL, NULL) < 0) {
			// Se ho ricevuto un segnale SIGUSR2 controllo se uno dei worker ha
			// liberato un filedescriptor
			if (errno == EINTR && received_sigusr2) {
				received_sigusr2 = false;
				// Controlla quali ack sono a 1 (eventualmente nessuno, in caso
				// di segnali spurii)
				for (int i = 0; i < ThreadsInPool; ++i) {
					if (freefd_ack[i] == 1) {
						// Aggiunge freefd[i] alla bitmap su cui esegue la select
						FD_SET(freefd[i], &set);
						// Non c'è bisogno di aggiornare fd_num perché un fd
						// arrivato da un worker è stato accettato da listener,
						// quindi ha già modificato fd_num
						freefd_ack[i] = 0;
					}
				}
			}
			else
				perror("select del listener");
		}
		else {
			// Select terminata correttamente: controlla quale fd è pronto
			for (int fd = 0; fd <= fdnum; ++fd) {
				if (FD_ISSET(fd, &rset)) {
					if (fd == ssfd) {
						// Richiesta di nuova connessione
						int newfd = accept(ssfd, NULL, 0);
						#ifdef DEBUG
							fprintf(stderr, "Richiesta di nuova connessione: %d\n", newfd);
						#endif
						// Accetta al massimo MaxConnections dai client
						if (newfd < MaxConnections) {
							FD_SET(newfd, &set);
							// Non serve la lock perché il listener è l'unico che
							// modifica fdnum
							if (newfd > fdnum) {
								fdnum = newfd;
							}
						}
						else {
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

	return NULL;
}



/**
 * @brief Modifica le strutture dati necessarie alla disconnessione di un client
 * dal fd passato. Se il fd passato non ha associato nessun client, viene
 * solamente chiuso il fd.
 *
 * Il fd passato DEVE essere aperto e gestito dal worker attuale (perché questo
 * garantisce la sincronizzazione, vedere la relazione).
 *
 * @param fd Il fd su cui lavorare
 */
void disconnectClient(int fd) {
	if (fd_to_nickname[fd] == NULL) {
		close(fd);
		return;
	}
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
	close(fd);
}

/**
 * @brief Modifica le strutture dati necessarie per gestire la connessione di un
 * client.
 *
 * Si aspetta che sia già stata acquisita la lock connected_mutex.
 *
 * @param nick Il nickname che si è connesso.
 * @param fd Il fd su cui si è connesso.
 * @param nick_data La struttura nickname_t associata a quel nickname in
 *                  nickname_htable.
 */
void connectClient(char* nick, int fd, nickname_t* nick_data) {
	error_handling_lock(&(nick_data->mutex));
	nick_data->fd = fd;
	error_handling_unlock(&(nick_data->mutex));
	fd_to_nickname[fd] = malloc((strlen(nick) + 1) * sizeof(char*));
	strncpy(fd_to_nickname[fd], nick, strlen(nick) + 1);
	++num_connected;
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
 * @brief Invia un messaggio come risposta ad una richiesta di un client.
 *
 * @param fd Il fd su cui inviare la risposta.
 * @param res Il messaggio da inviare come risposta.
 * @return Il valore da assegnare a fdclose (true se il client si è disconnesso,
           false altrimenti). Se ritorna true, sendMsgResponse disconnette anche
		   il client.
 */
bool sendMsgResponse(int fd, message_t* res) {
	#ifdef DEBUG
		fprintf(stderr, "Invio risposta (msg) al client\n");
	#endif
	if (sendRequest(fd, res) < 0) {
		if (errno == EPIPE) {
			// Client disconnesso
			disconnectClient(fd);
			return true;
		}
		else {
			perror("inviando un messaggio");
		}
	}
	return false;
}

/**
 * @brief Invia un header come risposta ad una richiesta di un client.
 *
 * @param fd Il fd su cui inviare la risposta.
 * @param res L'header da inviare.
 * @return Il valore da assegnare a fdclose (true se il client si è disconnesso,
           false altrimenti). Se ritorna true, sendHdrResponse disconnette anche
           il client.
 */
bool sendHdrResponse(int fd, message_hdr_t* res) {
	#ifdef DEBUG
		fprintf(stderr, "Invio risposta (hdr) al client\n");
	#endif
	if (sendHeader(fd, res) < 0) {
		if (errno == EPIPE) {
			// Client disconnesso
			disconnectClient(fd);
			return true;
		}
		else {
			perror("inviando un messaggio");
		}
	}
	return false;
}

/**
 * @brief Funzione che verifica i dati del client prima di eseguire le
 * richieste che richiedono di essere connessi.
 * Se il client non è "regolare" gli invia la risposta di fallimento.
 *
 * @param nick Il nickname
 * @param fd Il fd da cui arriva la richiesta
 * @param nick_data Il nickname_t associato al nickname passato
 * @return true se il client è regolare, altrimenti false
 */
bool checkConnected(char* nick, int fd, nickname_t* nick_data) {
	message_t response;
	if (nick_data == NULL) {
		// nickname sconosciuto
		#ifdef DEBUG
			fprintf(stderr, "Richiesta di operazione da un nickname inesistente\n");
		#endif
		setHeader(&response.hdr, OP_NICK_UNKNOWN, "");
		if (!sendHdrResponse(fd, &response.hdr))
			disconnectClient(fd);
		return false;
	}
	if (nick_data->fd != fd) {
		#ifdef DEBUG
			fprintf(stderr, "Richiesta su un fd diverso da quello del nickname\n");
		#endif
		setHeader(&response.hdr, OP_FAIL, "");
		if (!sendHdrResponse(fd, &response.hdr))
			disconnectClient(fd);
		return false;
	}
	return true;
}

/**
 * @brief Verifica che un messaggio abbia dati corretti.
 *
 * @param msg Il messaggio da controllare
 * @return true se il messaggio è regolare, altrimenti false
 */
bool checkMsg(message_t* msg) {
	if (msg->data.hdr.len == (strlen(msg->data.buf) + 1))
		return true;
	else
		return false;
}

/**
 * @brief main di un thread worker, che esegue una operazione alla volta
 *
 * I thread worker si mettono in attesa sulla coda condivisa per delle
 * operazioni da svolgere; non appena ne ricevono una la eseguono, rispondendo
 * al client che l'ha richiesta.
 *
 * @param arg il proprio numero d'indice
 */
void* worker_thread(void* arg) {
	int workerNumber = *(int*)arg;

	while(threads_continue) {
		int localfd = ts_pop(&queue);
		if (localfd == TERMINATION_FD) {
			// Ha ricevuto il fd falso passato dal signal_handler_thread
			break;
		}
		message_t msg;
		bool fdclose = false;
		// Le comunicazioni iniziano sempre con un messaggio
		int readResult = readMsg(localfd, &msg);
		if (readResult < 0) {
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
		else if (readResult == 0) {
			// Client disconnesso
			disconnectClient(localfd);
			fdclose = true;
		}
		else {
			message_t response;
			nickname_t* sender;
			switch (msg.hdr.op) {
				case REGISTER_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta REGISTER_OP\n", workerNumber);
					#endif
					if ((sender = ts_hash_insert(nickname_htable, msg.hdr.sender)) == NULL) {
						// Nickname già esistente
						#ifdef DEBUG
							fprintf(stderr, "%d: Nickname %s già esistente!\n", workerNumber, msg.hdr.sender);
						#endif
						setHeader(&response.hdr, OP_NICK_ALREADY, "");
						if (!sendHdrResponse(localfd, &response.hdr))
							disconnectClient(localfd);
						fdclose = true;
					}
					else {
						// Situazione normale
						#ifdef DEBUG
							fprintf(stderr, "%d: Registrato il nickname \"%s\"\n", workerNumber, msg.hdr.sender);
						#endif
						pthread_mutex_lock(&connected_mutex);
						connectClient(msg.hdr.sender, localfd, sender);
						responseConnectedList(&response);
						pthread_mutex_unlock(&connected_mutex);
						fdclose = sendMsgResponse(localfd, &response);
						free(response.data.buf);
					}
				}
				break;
				case CONNECT_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta CONNECT_OP\n", workerNumber);
					#endif
					if ((sender = ts_hash_find(nickname_htable, msg.hdr.sender)) != NULL) {
						error_handling_lock(&(sender->mutex));
						if (sender->fd != 0) {
							// Nickname già connesso
							error_handling_unlock(&(sender->mutex));
							#ifdef DEBUG
								fprintf(stderr, "%d: Nick \"%s\" già connesso!\n", workerNumber, msg.hdr.sender);
							#endif
							setHeader(&response.hdr, OP_NICK_CONN, "");
							if (!sendHdrResponse(localfd, &response.hdr))
								disconnectClient(localfd);
							fdclose = true;
						}
						else {
							// Situazione normale
							error_handling_unlock(&(sender->mutex));
							#ifdef DEBUG
								fprintf(stderr, "%d: Connesso \"%s\" (fd %d)\n", workerNumber, msg.hdr.sender, localfd);
							#endif
							error_handling_lock(&connected_mutex);
							connectClient(msg.hdr.sender, localfd, sender);
							responseConnectedList(&response);
							error_handling_unlock(&connected_mutex);
							fdclose = sendMsgResponse(localfd, &response);
							free(response.data.buf);
						}
					}
					else {
						// Nickname inesistente
						#ifdef DEBUG
							fprintf(stderr, "%d: Richiesta di connessione di un nickname inesistente\n", workerNumber);
						#endif
						setHeader(&response.hdr, OP_NICK_UNKNOWN, "");
						if (!sendHdrResponse(localfd, &response.hdr))
							disconnectClient(localfd);
						fdclose = true;
					}
				}
				break;
				case USRLIST_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta USRLIST_OP\n", workerNumber);
					#endif
					pthread_mutex_lock(&connected_mutex);
					responseConnectedList(&response);
					pthread_mutex_unlock(&connected_mutex);
					fdclose = sendMsgResponse(localfd, &response);
					free(response.data.buf);
				}
				break;
				case POSTTXT_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta POSTTXT_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, ts_hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						if (!checkMsg(&msg)) {
							// Messaggio invalido
							setHeader(&response.hdr, OP_MSG_INVALID, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
						}
						else if (msg.data.hdr.len > MaxMsgSize) {
							// Messaggio troppo lungo
							setHeader(&response.hdr, OP_MSG_TOOLONG, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
						}
						else {
							// Situazione normale
							msg.hdr.op = TXT_MESSAGE;
							nickname_t* receiver = ts_hash_find(nickname_htable, msg.data.hdr.receiver);
							add_to_history(receiver, msg);
							error_handling_lock(&(receiver->mutex));
							if (receiver->fd > 0) {
								// Non fa gestione dell'errore perché se non
								// riesce ad inviare è un problema del client,
								// il server se lo tiene nell'history e poi sarà
								// il client a chiedergli di nuovo il messaggio.
								sendRequest(receiver->fd, &msg);
							}
							error_handling_unlock(&(receiver->mutex));
							setHeader(&response.hdr, OP_OK, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
						}
					}
				}
				break;
				case GETPREVMSGS_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta GETPREVMSGS_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, sender = ts_hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare, situazione normale
						setHeader(&response.hdr, OP_OK, "");
						error_handling_lock(&(sender->mutex));
						size_t nmsgs = history_len(sender);
						setData(&response.data, "", (char*)&nmsgs, sizeof(size_t));
						fdclose = sendMsgResponse(localfd, &response);
						if (!fdclose) {
							int i;
							message_t* curr_msg;
							history_foreach(sender, i, curr_msg) {
								if (sendMsgResponse(localfd, curr_msg)) {
									fdclose = true;
									break;
								}
							}
						}
						error_handling_unlock(&(sender->mutex));
					}
				}
				break;
				case POSTTXTALL_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta POSTTXTALL_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, sender = ts_hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						if (!checkMsg(&msg)) {
							// Messaggio invalido
							setHeader(&response.hdr, OP_MSG_INVALID, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
						}
						else {
							// Situazione normale
							msg.hdr.op = TXT_MESSAGE;
							int i;
							icl_entry_t* j;
							char* key;
							nickname_t* val;
							icl_hash_foreach(nickname_htable->htable, i, j, key, val) {
								add_to_history(val, msg);
								error_handling_lock(&(val->mutex));
								if (val->fd > 0) {
									sendRequest(val->fd, &msg);
								}
								error_handling_unlock(&(val->mutex));
							}
							setHeader(&response.hdr, OP_OK, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
						}
					}
				}
				break;
				default: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta operazione sconosciuta\n", workerNumber);
					#endif
					setHeader(&response.hdr, OP_FAIL, "");
					if (!sendHdrResponse(localfd, &response.hdr))
						disconnectClient(localfd);
					fdclose = true;
				}
				break;
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
	// Assert su una costante globale
	assert(TERMINATION_FD < 0);
	assert(NULL == 0);

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
	ThreadsInPool = 8;
	int MaxHistMsgs = 16;
	MaxMsgSize = 512;
	MaxFileSize = 1024;
	MaxConnections = 32;
	char* UnixPath = "/tmp/chatty_socket";
	char* DirName = "/tmp/chatty";
	char* StatFileName = "/tmp/chatty_stats.txt";

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
	MaxConnections += 4; // Serve solo aumentata
	queue = create_fifo();
	nickname_htable = hash_create(NICKNAME_HASH_BUCKETS_N, MaxHistMsgs);
	fdnum = createSocket(UnixPath);
	assert(MaxConnections > fdnum);
	pthread_t pool[ThreadsInPool];
	freefd = malloc(ThreadsInPool * sizeof(int));
	freefd_ack = calloc(ThreadsInPool, sizeof(char));
	pthread_mutex_init(&connected_mutex, NULL);
	fd_to_nickname = calloc(MaxConnections, sizeof(char*));
	// Crea i vari thread
	pthread_create(&listener, NULL, &listener_thread, NULL);
	for (unsigned int i = 0; i < ThreadsInPool; ++i) {
		// ricicla lo spazio di freefd per passare ai worker il loro numero
		freefd[i] = i;
		pthread_create(pool + i, NULL, &worker_thread, freefd + i);
	}
	// Diventa il thread che gestisce i segnali
	signal_handler_thread();
	// Se signal_handler_thread ritorna vuol dire che deve aspettare gli altri
	// thread, eseguire i cleanup finali e poi terminare
	// In teoria queste chiamate non possono ritornare errore (gli errori
	// segnati in man pthread_join non possono verificarsi in questi casi)
	pthread_join(listener, NULL);
	for (unsigned int i = 0; i < ThreadsInPool; ++i) {
		pthread_join(pool[i], NULL);
	}

	// Elimina il socket
	#ifdef DEBUG
		fprintf(stderr, "Unlink del socket\n");
	#endif
	unlink(UnixPath);
	// Libera la memoria che ha occupato all'inizio del programma
	#ifdef DEBUG
		fprintf(stderr, "Cancello la coda condivisa\n");
	#endif
	clear_fifo(&queue);
	#ifdef DEBUG
		fprintf(stderr, "Libero gli array di comunicazione listener-worker\n");
	#endif
	free(freefd);
	free(freefd_ack);
	// libera tutti i valori inizializzati di fd_to_nickname
	#ifdef DEBUG
		fprintf(stderr, "Svuoto fd_to_nickname\n");
	#endif
	for (int i = 0; i < fdnum; ++i) {
		if (fd_to_nickname[i] != NULL) {
			free(fd_to_nickname[i]);
		}
	}
	free(fd_to_nickname);
	// Non ci sono altri thread oltre a main, quindi nessuno ha il lock
	pthread_mutex_destroy(&connected_mutex);
	#ifdef DEBUG
		fprintf(stderr, "Elimino l'hashtable\n");
	#endif
	ts_hash_destroy(nickname_htable);

	return 0;
}
