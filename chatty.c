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

#define NICKNAME_HASH_BUCKETS_N 100000
#define TERMINATION_FD -1
#define FILE_SIZE_FACTOR 1024
#define CONFIG_LINE_LENGTH 1024

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
int MaxHistMsgs;
int MaxMsgSize;
int MaxFileSize;
int MaxConnections;
char* DirName;
char* StatFileName;
char* UnixPath;

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
	int statsfd = MaxConnections + ThreadsInPool + 1;

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
				#if defined VERBOSE
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
			#endif
			// gestire il segnale
			int filefd = open(StatFileName, O_WRONLY | O_CREAT | O_APPEND, 0744);
			if (filefd < 0
				|| dup2(filefd, statsfd) < 0) {
				perror("aprendo il file delle statistiche");
			}
			else {
				close(filefd);
				if (dprintf(statsfd, "<%ld - %d %d %ld %ld %ld %ld %ld >\n",
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
	sigemptyset(&sigusr2_action.sa_mask);
	sigusr2_action.sa_flags = 0;
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
						#if defined DEBUG && defined VERBOSE
							fprintf(stderr, "Ricevuto un fd da un worker\n");
						#endif
					}
				}
			}
			else
				perror("select del listener");
		}
		else {
			// Select terminata correttamente: controlla quale fd è pronto
			#if defined DEBUG && defined VERBOSE
                fprintf(stderr, "Ricevuto qualcosa dalla select\n");
            #endif
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
							++chattyStats.nerrors;
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
	nickname_t* client = hash_find(nickname_htable, fd_to_nickname[fd]);
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
		#ifdef VERBOSE
			fprintf(stderr, "p: %d, num_connected: %d\n", p, num_connected);
		#endif
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
	#if defined DEBUG && defined VERBOSE
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
	#if defined DEBUG && defined VERBOSE
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
 * Per client regolare si intende un client con un nickname registrato che manda
 * richieste dal fd su cui è stata fatta una CONNECT_OP con quel nickname. Per
 * nickname del client si intende quello ricevuto come msg.hdr.sender.
 *
 * @param nick Il nickname da cui arriva la richiesta (msg.hdr.sender)
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
		++chattyStats.nerrors;
		return false;
	}
	if (nick_data->fd != fd) {
		#ifdef DEBUG
			fprintf(stderr, "Richiesta su un fd diverso da quello del nickname\n");
		#endif
		setHeader(&response.hdr, OP_FAIL, "");
		if (!sendHdrResponse(fd, &response.hdr))
			disconnectClient(fd);
		++chattyStats.nerrors;
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
		msg.data.buf = NULL;
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
						++chattyStats.nerrors;
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
				case UNREGISTER_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta UNREGISTER_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						#ifdef DEBUG
							fprintf(stderr, "%d: Deregistro il nickname \"%s\"\n", workerNumber, msg.hdr.sender);
						#endif
						setHeader(&response.hdr, OP_OK, "");
						sendHdrResponse(localfd, &response.hdr);
						// Un client che deregistra un nick non può restare
						// connesso con quel nickname
						disconnectClient(localfd);
						fdclose = true;
						ts_hash_remove(nickname_htable, msg.hdr.sender);
					}
				}
				break;
				case CONNECT_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta CONNECT_OP\n", workerNumber);
					#endif
					if ((sender = hash_find(nickname_htable, msg.hdr.sender)) != NULL) {
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
							++chattyStats.nerrors;
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
						++chattyStats.nerrors;
						fdclose = true;
					}
				}
				break;
				case DISCONNECT_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta DISCONNECT_OP\n", workerNumber);
						fprintf(stderr, "%d: Disconnessione fd %d (\"%s\")\n", workerNumber, localfd, fd_to_nickname[localfd]);
					#endif
					disconnectClient(localfd);
					fdclose = true;
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
					fdclose = !checkConnected(msg.hdr.sender, localfd, hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						if (!checkMsg(&msg)) {
							// Messaggio invalido
							setHeader(&response.hdr, OP_MSG_INVALID, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
							++chattyStats.nerrors;
						}
						else if (msg.data.hdr.len > MaxMsgSize) {
							// Messaggio troppo lungo
							setHeader(&response.hdr, OP_MSG_TOOLONG, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
							++chattyStats.nerrors;
						}
						else {
							nickname_t* receiver = hash_find(nickname_htable, msg.data.hdr.receiver);
							if (receiver == NULL) {
								// Destinatario inesistente
								setHeader(&response.hdr, OP_DEST_UNKNOWN, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							else {
								// Situazione normale
								msg.hdr.op = TXT_MESSAGE;
								add_to_history(receiver, msg);
								error_handling_lock(&(receiver->mutex));
								if (receiver->fd > 0) {
									// Non fa gestione dell'errore perché se non
									// riesce ad inviare è un problema del client,
									// il server se lo tiene nell'history e poi sarà
									// il client a chiedergli di nuovo il messaggio.
									sendRequest(receiver->fd, &msg);
									++chattyStats.ndelivered;
								}
								else {
									++chattyStats.nnotdelivered;
								}
								error_handling_unlock(&(receiver->mutex));
								// Mette a NULL in modo che non venga deallocato
								msg.data.buf = NULL;
								setHeader(&response.hdr, OP_OK, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
							}
						}
					}
				}
				break;
				case POSTTXTALL_OP: {
					#ifdef DEBUG
					fprintf(stderr, "%d: Ricevuta POSTTXTALL_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, sender = hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						if (!checkMsg(&msg)) {
							// Messaggio invalido
							setHeader(&response.hdr, OP_MSG_INVALID, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
							++chattyStats.nerrors;
						}
						else {
							// Situazione normale
							msg.hdr.op = TXT_MESSAGE;
							int i;
							icl_entry_t* j;
							char* key;
							nickname_t* val;
							char* original_buffer = msg.data.buf;
							icl_hash_foreach(nickname_htable->htable, i, j, key, val) {
								// Copia il buffer perché ogni history può
								// cancellare il messaggio (con conseguente free
								// del buffer) separatamente, quindi deve essere
								// un puntatore diverso.
								// TODO: implementare un puntatore
								// multiriferimento che fa la free solo quando
								// viene cancellato l'ultimo
								msg.data.buf = malloc(msg.data.hdr.len * sizeof(char));
								strncpy(msg.data.buf, original_buffer, msg.data.hdr.len);
								add_to_history(val, msg);
								error_handling_lock(&(val->mutex));
								if (val->fd > 0) {
									sendRequest(val->fd, &msg);
									++chattyStats.ndelivered;
								}
								else {
									++chattyStats.nnotdelivered;
								}
								error_handling_unlock(&(val->mutex));
							}
							msg.data.buf = original_buffer;
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
					fdclose = !checkConnected(msg.hdr.sender, localfd, sender = hash_find(nickname_htable, msg.hdr.sender));
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
				case POSTFILE_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta POSTFILE_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						nickname_t* receiver = hash_find(nickname_htable, msg.data.hdr.receiver);
						if (receiver == NULL) {
							// Destinatario inesistente
							setHeader(&response.hdr, OP_DEST_UNKNOWN, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
							++chattyStats.nerrors;
						}
						else {
							// Situazione normale
							// Crea e apre il file (così se succedono errori può
							// esplodere subito)
							message_data_t file;
							file.buf = NULL;
							char* full_filename = malloc(strlen(DirName) + msg.data.hdr.len);
							strncpy(full_filename, DirName, strlen(DirName));
							strncpy(full_filename + strlen(DirName), msg.data.buf, msg.data.hdr.len);
							#ifdef DEBUG
								fprintf(stderr, "%d: salvo il file \"%s\"\n", workerNumber, full_filename);
							#endif
							int filefd = open(full_filename, O_WRONLY | O_CREAT, 0755);
							if (filefd < 0
								|| dup2(filefd, MaxConnections + workerNumber) < 0) {
								perror("aprendo il file");
								setHeader(&response.hdr, OP_FAIL, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							// Scarica il file
							else {
								close(filefd);
								if (readData(localfd, &file) <= 0) {
									perror("scaricando un file");
									setHeader(&response.hdr, OP_FAIL, "");
									fdclose = sendHdrResponse(localfd, &response.hdr);
									++chattyStats.nerrors;
								}
								// Salva il file
								else if (file.hdr.len > MaxFileSize * FILE_SIZE_FACTOR) {
									// File troppo grosso
									setHeader(&response.hdr, OP_MSG_TOOLONG, "");
									fdclose = sendHdrResponse(localfd, &response.hdr);
									++chattyStats.nerrors;
								}
								else if (write(MaxConnections + workerNumber, file.buf, file.hdr.len) < 0) {
									perror("writing to output file");
									setHeader(&response.hdr, OP_FAIL, "");
									fdclose = sendHdrResponse(localfd, &response.hdr);
									++chattyStats.nerrors;
								}
								else {
									// È andato tutto bene
									msg.hdr.op = FILE_MESSAGE;
									add_to_history(receiver, msg);
									error_handling_lock(&(receiver->mutex));
									if (receiver->fd > 0) {
										// Non fa gestione dell'errore perché se non
										// riesce ad inviare è un problema del client,
										// il server se lo tiene nell'history e poi sarà
										// il client a chiedergli di nuovo il messaggio.
										sendRequest(receiver->fd, &msg);
										// Non aumenta i file consegnati perché
										// viene fatto quando finisce GETFILE_OP
									}
									else {
										++chattyStats.nfilenotdelivered;
									}
									error_handling_unlock(&(receiver->mutex));
									// Mette a NULL in modo che non venga deallocato
									msg.data.buf = NULL;
									setHeader(&response.hdr, OP_OK, "");
									fdclose = sendHdrResponse(localfd, &response.hdr);
								}
								close(MaxConnections + workerNumber);
								free(full_filename);
								if (file.buf != NULL) {
									free(file.buf);
								}
							}
						}
					}
				}
				break;
				case GETFILE_OP: {
					#ifdef DEBUG
						fprintf(stderr, "%d: Ricevuta GETFILE_OP\n", workerNumber);
					#endif
					fdclose = !checkConnected(msg.hdr.sender, localfd, hash_find(nickname_htable, msg.hdr.sender));
					if (!fdclose) {
						// Client regolare
						char* mappedfile = NULL;
						struct stat st;
						// Apre il file
						char* full_filename = malloc(strlen(DirName) + msg.data.hdr.len);
						strncpy(full_filename, DirName, strlen(DirName));
						strncpy(full_filename + strlen(DirName), msg.data.buf, msg.data.hdr.len);
						#ifdef DEBUG
							fprintf(stderr, "%d: apro il file \"%s\"\n", workerNumber, full_filename);
						#endif
						int filefd = open(full_filename, O_RDONLY);
						if (filefd < 0) {
							if (errno == EACCES) {
								// File inesistente
								#ifdef DEBUG
									fprintf(stderr, "%d: il file richiesto non esiste\n", workerNumber);
								#endif
								setHeader(&response.hdr, OP_NO_SUCH_FILE, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							else {
								perror("aprendo il file");
								setHeader(&response.hdr, OP_FAIL, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
						}
						else if (dup2(filefd, MaxConnections + workerNumber) < 0) {
							perror("aprendo il file");
							setHeader(&response.hdr, OP_FAIL, "");
							fdclose = sendHdrResponse(localfd, &response.hdr);
							++chattyStats.nerrors;
						}
						// Legge la lunghezza del file
						else {
							close(filefd);
							if (stat(full_filename, &st) < 0) {
								perror("stat");
								setHeader(&response.hdr, OP_FAIL, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							else if (!S_ISREG(st.st_mode)) {
								fprintf(stderr, "ERRORE: il file %s non e' un file regolare\n", msg.data.buf);
								setHeader(&response.hdr, OP_FAIL, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							// Legge il file in memoria
							else if ((mappedfile = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, MaxConnections + workerNumber, 0)) == MAP_FAILED) {
								perror("mmap");
								fprintf(stderr, "ERRORE: mappando il file %s in memoria\n", msg.data.buf);
								setHeader(&response.hdr, OP_FAIL, "");
								fdclose = sendHdrResponse(localfd, &response.hdr);
								++chattyStats.nerrors;
							}
							else {
								// È andato tutto bene
								setHeader(&response.hdr, OP_OK, "");
								setData(&response.data, "", mappedfile, st.st_size);
								fdclose = sendMsgResponse(localfd, &response);
								++chattyStats.nfiledelivered;
							}
							close(MaxConnections + workerNumber);
							free(full_filename);
							if (mappedfile != NULL) {
								if (munmap(mappedfile, st.st_size) < 0) {
									perror("errore durante munmap del file");
									exit(EXIT_FAILURE);
								}
							}
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
		if (msg.data.buf != NULL) {
			free(msg.data.buf);
		}
		// Finita la richiesta segnala al listener che il fd è di nuovo libero
		// se non ha chiuso la connessione
		#if defined DEBUG && defined VERBOSE
			fprintf(stderr, "Operazione gestita, inizio comunicazione con il listener\n");
		#endif
		if (!fdclose) {
			while(freefd_ack[workerNumber] == 1) {
				pthread_kill(listener, SIGUSR2);
				sched_yield();
			}
			freefd[workerNumber] = localfd;
			freefd_ack[workerNumber] = 1;
			pthread_kill(listener, SIGUSR2);
			#if defined DEBUG && defined VERBOSE
				fprintf(stderr, "%d: Restituito l'fd al listener\n", workerNumber);
			#endif
		}
	}

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
				if (strncmp(paramName, "ThreadsInPool", 14) == 0) {
					ThreadsInPool = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto ThreadsInPool: %d\n", ThreadsInPool);
					#endif
				}
				else if (strncmp(paramName, "MaxHistMsgs", 12) == 0) {
					MaxHistMsgs = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxHistMsgs: %d\n", MaxHistMsgs);
					#endif
				}
				else if (strncmp(paramName, "MaxMsgSize", 11) == 0) {
					MaxMsgSize = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxMsgSize: %d\n", MaxMsgSize);
					#endif
				}
				else if (strncmp(paramName, "MaxFileSize", 12) == 0) {
					MaxFileSize = strtol(paramValue, NULL, 10);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxFileSize: %d\n", MaxFileSize);
					#endif
				}
				else if (strncmp(paramName, "MaxConnections", 15) == 0) {
					MaxConnections = strtol(paramValue, NULL, 10);
					MaxConnections += 4; // Serve solo aumentata
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto MaxConnections: %d\n", MaxConnections);
					#endif
				}
				else if (strncmp(paramName, "UnixPath", 9) == 0) {
					UnixPath = malloc((strlen(paramValue) + 1) * sizeof(char));
					strncpy(UnixPath, paramValue, strlen(paramValue) + 1);
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto UnixPath: %s\n", UnixPath);
					#endif
				}
				else if (strncmp(paramName, "DirName", 8) == 0) {
					DirName = malloc((strlen(paramValue) + 2) * sizeof(char));
					strncpy(DirName, paramValue, strlen(paramValue));
					// Serve solo con lo / finale
					DirName[strlen(paramValue)] = '/';
					DirName[strlen(paramValue) + 1] = '\0';
					#if defined DEBUG && defined VERBOSE
						fprintf(stderr, "Letto DirName: %s\n", DirName);
					#endif
				}
				else if (strncmp(paramName, "StatFileName", 13) == 0) {
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
	for (int i = 0; i < fdnum; ++i) {
		if (fd_to_nickname[i] != NULL) {
			free(fd_to_nickname[i]);
		}
	}
	free(fd_to_nickname);
	// Non ci sono altri thread oltre a main, quindi nessuno ha il lock
	pthread_mutex_destroy(&connected_mutex);
	#if defined DEBUG && defined VERBOSE
		fprintf(stderr, "Elimino l'hashtable\n");
	#endif
	ts_hash_destroy(nickname_htable);

	return 0;
}
