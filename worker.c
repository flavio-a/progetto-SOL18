/**
 * membox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 * @file worker.c
 * @brief File contenente il codice dei thread worker
 *
 * Si dichiara che il contenuto di questo file è in ogni sua parte opera
 * originale dell'autore.
 *
 * @author Flavio Ascari
 *		 550341
 *       flavio.ascari@sns.it
 */
#include "worker.h"


// ------------------------- funzioni interne -----------------------

/**
 * @brief Risponde con un errore ad un client, poi chiude la connessione.
 *
 * @param response (message_t) Il messaggio da inviare come risposta
 * @param fd (int) Il fd su cui rispondere
 * @param err_op (op_t) Il tipo di errore
 */
#define sendFatalFailResponse(response, fd, err_op) \
	setHeader(&response.hdr, err_op, ""); \
	if (!sendHdrResponse(fd, &response.hdr)) \
		disconnectClient(fd); \
	increaseStat(nerrors)

/**
 * @brief Risponde con un errore ad un client, ma potrebbe non chiudere la connessione.
 *
 * @param response (message_t) Il messaggio da inviare come risposta
 * @param fd (int) Il fd su cui rispondere
 * @param err_op (op_t) Il tipo di errore
 * @param fdclose (bool) La variabile che deve contenere se il client si è disconnesso o meno
 */
#define sendSoftFailResponse(response, fd, err_op, fdclose) \
	setHeader(&response.hdr, err_op, ""); \
	fdclose = sendHdrResponse(fd, &response.hdr); \
	increaseStat(nerrors)

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
	for (int i = 0; i < MaxConnections; ++i) {
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
		sendFatalFailResponse(response, fd, OP_NICK_UNKNOWN);
		return false;
	}
	if (nick_data->fd != fd) {
		#ifdef DEBUG
			fprintf(stderr, "Richiesta su un fd diverso da quello del nickname\n");
		#endif
		sendFatalFailResponse(response, fd, OP_WRONG_FD);
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

// ------------------------- funzioni esportate -----------------------

// Documentata in worker.h
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
						sendFatalFailResponse(response, localfd, OP_NICK_ALREADY);
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
							sendFatalFailResponse(response, localfd, OP_NICK_CONN);
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
						sendFatalFailResponse(response, localfd, OP_NICK_UNKNOWN);
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
							sendSoftFailResponse(response, localfd, OP_MSG_INVALID, fdclose);
						}
						else if (msg.data.hdr.len > MaxMsgSize) {
							// Messaggio troppo lungo
							sendSoftFailResponse(response, localfd, OP_MSG_TOOLONG, fdclose);
						}
						else {
							nickname_t* receiver = hash_find(nickname_htable, msg.data.hdr.receiver);
							if (receiver == NULL) {
								// Destinatario inesistente
								sendSoftFailResponse(response, localfd, OP_DEST_UNKNOWN, fdclose);
							}
							else {
								// Situazione normale
								msg.hdr.op = TXT_MESSAGE;
								error_handling_lock(&(receiver->mutex));
								add_to_history(receiver, msg);
								if (receiver->fd > 0) {
									// Non fa gestione dell'errore perché se non
									// riesce ad inviare è un problema del client,
									// il server se lo tiene nell'history e poi sarà
									// il client a chiedergli di nuovo il messaggio.
									sendRequest(receiver->fd, &msg);
									error_handling_unlock(&(receiver->mutex));
									increaseStat(ndelivered);
								}
								else {
									error_handling_unlock(&(receiver->mutex));
									increaseStat(nnotdelivered);
								}
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
							sendSoftFailResponse(response, localfd, OP_MSG_INVALID, fdclose);
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
								error_handling_lock(&(val->mutex));
								add_to_history(val, msg);
								if (val->fd > 0) {
									sendRequest(val->fd, &msg);
									error_handling_unlock(&(val->mutex));
									increaseStat(ndelivered);
								}
								else {
									error_handling_unlock(&(val->mutex));
									increaseStat(nnotdelivered);
								}
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
							sendSoftFailResponse(response, localfd, OP_DEST_UNKNOWN, fdclose);
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
								sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
							}
							// Scarica il file
							else {
								close(filefd);
								if (readData(localfd, &file) <= 0) {
									perror("scaricando un file");
									sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
								}
								// Salva il file
								else if (file.hdr.len > MaxFileSize * FILE_SIZE_FACTOR) {
									// File troppo grosso
									sendSoftFailResponse(response, localfd, OP_MSG_TOOLONG, fdclose);
								}
								else if (write(MaxConnections + workerNumber, file.buf, file.hdr.len) < 0) {
									perror("writing to output file");
									sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
								}
								else {
									// È andato tutto bene
									msg.hdr.op = FILE_MESSAGE;
									error_handling_lock(&(receiver->mutex));
									add_to_history(receiver, msg);
									if (receiver->fd > 0) {
										// Non fa gestione dell'errore perché se non
										// riesce ad inviare è un problema del client,
										// il server se lo tiene nell'history e poi sarà
										// il client a chiedergli di nuovo il messaggio.
										sendRequest(receiver->fd, &msg);
										// Non aumenta i file consegnati perché
										// viene fatto quando finisce GETFILE_OP
										error_handling_unlock(&(receiver->mutex));
									}
									else {
										error_handling_unlock(&(receiver->mutex));
										increaseStat(nfilenotdelivered);
									}
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
								sendSoftFailResponse(response, localfd, OP_NO_SUCH_FILE, fdclose);
							}
							else {
								perror("aprendo il file");
								sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
							}
						}
						else if (dup2(filefd, MaxConnections + workerNumber) < 0) {
							perror("aprendo il file");
							close(filefd);
							sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
						}
						// Legge la lunghezza del file
						else {
							close(filefd);
							if (stat(full_filename, &st) < 0) {
								perror("stat");
								sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
							}
							else if (!S_ISREG(st.st_mode)) {
								fprintf(stderr, "ERRORE: il file %s non e' un file regolare\n", msg.data.buf);
								sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
							}
							// Legge il file in memoria
							else if ((mappedfile = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, MaxConnections + workerNumber, 0)) == MAP_FAILED) {
								perror("mmap");
								fprintf(stderr, "ERRORE: mappando il file %s in memoria\n", msg.data.buf);
								sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
							}
							else {
								// È andato tutto bene
								setHeader(&response.hdr, OP_OK, "");
								setData(&response.data, "", mappedfile, st.st_size);
								fdclose = sendMsgResponse(localfd, &response);
								increaseStat(nfiledelivered);
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
					sendSoftFailResponse(response, localfd, OP_FAIL, fdclose);
				}
				break;
			}
		}
		if (msg.data.buf != NULL) {
			free(msg.data.buf);
		}
		// Finita la richiesta segnala al listener che il fd è di nuovo libero
		// se non ha chiuso la connessione
		#ifdef DEBUG
			fprintf(stderr, "%d: Operazione gestita\n", workerNumber);
		#endif
		if (!fdclose) {
			#if defined DEBUG && defined VERBOSE
				fprintf(stderr, "%d: fd non chiuso, comunicazione con il listener\n", workerNumber);
			#endif
			while(freefd_ack[workerNumber] == 1) {
				pthread_kill(signal_handler, SIGUSR2);
				sched_yield();
			}
			freefd[workerNumber] = localfd;
			freefd_ack[workerNumber] = 1;
			pthread_kill(signal_handler, SIGUSR2);
			#if defined DEBUG && defined VERBOSE
				fprintf(stderr, "%d: Restituito l'fd al listener\n", workerNumber);
			#endif
		}
	}

	return NULL;
}
