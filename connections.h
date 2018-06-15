/*
 * chatterbox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 */
#ifndef CONNECTIONS_H_
#define CONNECTIONS_H_

#define MAX_RETRIES     10
#define MAX_SLEEPING     3
#if !defined(UNIX_PATH_MAX)
#define UNIX_PATH_MAX  64
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <message.h>

/**
 * @file  connections.h
 * @brief Contiene le funzioni che implementano il protocollo
 *        tra i clients ed il server
 *
 * La comunicazione avviene tramite scambio di messaggi. I messaggi sono di
 * tipo message_t, divisi in due parti (vedere la documentazione del tipo per
 * i dettagli).
 * Per trasmettere un messaggio, il protocollo prevede di inviare inizialmente
 * l'header del messaggio, poi se necessario il body. Per trasmettere il body
 * si scrivono in sequenza l'header del body (che contiene la dimensione del
 * buffer del body) e il buffer del body.
 *
 * Una comunicazione è strutturata nel seguente modo: il client invia al server
 * un messaggio, formato da header e body (anche se l'operazione non prevede un
 * body il client lo invia comunque, con lunghezza 0). Il server riceve ed
 * elabora il messaggio, poi risponde al client con solo un header la cui
 * operazione corrisponde all'esito della richiesta.
 * L'altra possibilità è che il server invii al client un intero messaggio (per
 * esempio a seguito di una richiesta di invio da parte di un altro client), nel
 * qual caso il client lo legge senza mandare nessun tipo di risposta al server.
 */

 // -------- connection handlers --------

/**
 * @function createSocket
 * @brief Crea un socket AF_UNIX su cui i client possono connettersi
 *
 * @param path Path del socket AF_UNIX
 *
 * @return il descrittore associato al socket in caso di successo
 *         < 0 in caso di errore
 */
int createSocket(char* path);

/**
 * @function openConnection
 * @brief Apre una connessione AF_UNIX verso il server
 *
 * @param path Path del socket AF_UNIX
 * @param ntimes numero massimo di tentativi di retry (deve essere <= MAX_RETRIES)
 * @param secs tempo di attesa tra due retry consecutive (deve essere <= MAX_SLEEPING)
 *
 * @return il descrittore associato alla connessione in caso di successo
 *         -1 in caso di errore
 */
int openConnection(char* path, unsigned int ntimes, unsigned int secs);

// -------- receiver side -----

/**
 * @function readHeader
 * @brief Legge l'header del messaggio
 *
 * @param fd     descrittore della connessione
 * @param hdr    puntatore su cui viene scritto l'header ricevuto
 *
 * @return <=0 se c'e' stato un errore
 *         (se <0 errno deve essere settato, se == 0 connessione chiusa)
 */
int readHeader(long fd, message_hdr_t *hdr);

/**
 * @function readData
 * @brief Legge il body del messaggio
 *
 * Questa funzione esegue in successione la lettura di header e body senza
 * controllare se qualcun altro ha già letto parte dei dati in mezzo. Prima
 * di chiamarla, assicurarsi che nessun altro thread sia in attesa su fd
 *
 * @param fd     descrittore della connessione
 * @param data   puntatore su cui viene scritto il body del messaggio
 *
 * @return <=0 se c'e' stato un errore
 *         (se <0 errno deve essere settato, se == 0 connessione chiusa)
 */
int readData(long fd, message_data_t *data);

/**
 * @function readMsg
 * @brief Legge l'intero messaggio
 *
 * Questa funzione esegue in successione la lettura di header e body senza
 * controllare se qualcun altro ha già letto parte dei dati in mezzo. Prima
 * di chiamarla, assicurarsi che nessun altro thread sia in attesa su fd.
 *
 * @param fd     descrittore della connessione
 * @param msg   puntatore su cui viene scritto il messaggio
 *
 * @return <=0 se c'e' stato un errore
 *         (se <0 errno deve essere settato, se == 0 connessione chiusa)
 */
int readMsg(long fd, message_t *msg);


// ------- sender side ------

/**
 * @function sendRequest
 * @brief Invia un messaggio di richiesta al server.
 *
 * Viene garantito che inviando un messaggio in questo modo sia possibile
 * leggerlo correttamente sia con una chiamata readMsg che con le due chiamate
 * consecutive di readHeader e readData.
 *
 * @param fd     descrittore della connessione
 * @param msg    puntatore al messaggio da inviare
 *
 * @return <=0 se c'e' stato un errore
 */
int sendRequest(long fd, message_t *msg);

/**
 * @function sendData
 * @brief Invia il body del messaggio al server
 *
 * @param fd     descrittore della connessione
 * @param data    puntatore al body del messaggio da inviare
 *
 * @return <=0 se c'e' stato un errore
 */
int sendData(long fd, message_data_t *data);


#endif /* CONNECTIONS_H_ */
