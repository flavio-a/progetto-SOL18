/*
 * chatterbox Progetto del corso di LSO 2017/2018
 *
 * Dipartimento di Informatica Università di Pisa
 * Docenti: Prencipe, Torquati
 *
 */
#ifndef MESSAGE_H_
#define MESSAGE_H_

#include <assert.h>
#include <string.h>
#include <config.h>
#include <ops.h>

/**
 * @file  message.h
 * @brief Libreria per i messaggi
 *
 * Questo file contiene il formato dei messaggi message_t insieme ad alcune
 * funzioni di utilità per manipolare questo formato
 */


/**
 * @struct message_hdr_t
 * @brief Questa struct contiene l'header di un messaggio.
 * Viene utilizzata come campo di message_t
 *
 * @var message_hdr_t::op
 * tipo di operazione richiesta al server
 * @var message_hdr_t::sender
 * nickname del mittente
 */
typedef struct {
    op_t     op;
    char sender[MAX_NAME_LENGTH+1];
} message_hdr_t;

/**
 * @struct message_data_hdr_t
 * @brief Questa struct contiene l'header dei dati di un messaggio
 * Viene usata come campo di message_data_t
 *
 * @var message_data_hdr_t::receiver
 * nickname del ricevente
 * @var message_data_hdr_t::len
 * lunghezza del buffer dati in byte
 */
typedef struct {
    char receiver[MAX_NAME_LENGTH+1];
    unsigned int   len;
} message_data_hdr_t;

/**
 * @struct message_data_t
 * @brief Questa struct contiene i dati del messaggio e il loro header
 * Viene usata come campo di message_t
 *
 * @var message_data_t::hdr
 * header dei dati
 * @var message_data_t::buf
 * buffer contenente i dati da inviare
 */
typedef struct {
    message_data_hdr_t  hdr;
    char               *buf;
} message_data_t;

/**
 * @struct message_t
 * @brief Questa struct rappresenta un messaggio scambiato tra client e server
 *
 * È divisa in due parti.
 * La prima è l'header del messaggio, di tipo message_hdr_t, che contiene il
 * tipo di operazione e il mittente.
 * La seconda è il body del messaggio, di tipo message_data_t, a sua volta
 * divisa in due parti: un header della parte dati, di tipo message_data_hdr_t,
 * ed un puntatore ad un buffer, la cui lunghezza è indicata nell'header, che
 * contiene proprio i dati da inviare
 *
 * I client inviano al server messaggi che hanno come op una di quelle permesse.
 * Il formato delle richieste dipende dall'operazione:
 * - REGISTER_OP: conta solo msg.hdr.sender, che contiene il nick da registrare
 * - UNREGISTER_OP: conta solo msg.hdr.sender, che deve corrispondere a quello
                    usato per la precedente operazione di connessione
 * - CONNECT_OP: conta solo msg.hdr.sender, che contiene il nickname con cui
                 connettersi
 * - DISCONNECT_OP: non serve nessuna informazione
 * - USRLIST_OP: non serve nessuna informazione
 * - POSTTXT_OP: msg.hdr.sender deve essere il proprio nick (con cui ci si è
                 connessi precedentemente), msg.data.hdr.receiver è il nick di
                 chi deve ricevere il messaggio, msg.data.hdr.len è la lunghezza
                 del buffer (incluso il terminatore \0), msg.data.buf deve
                 essere una stringa C valida (quindi con il terminatore \0)
 * - POSTTXTALL_OP: msg.hdr.sender deve essere il proprio nick (con cui ci si è
                    connessi precedentemente), msg.data.hdr.len è la lunghezza
                    del buffer (incluso il terminatore \0), msg.data.buf deve
                    essere una stringa C valida
 * - GETPREVMSGS_OP: conta solo msg.hdr.sender, che però deve essere il proprio
                     nick (con cui ci si è connessi precedentemente)
 * - POSTFILE_OP: msg.hdr.sender deve essere il proprio nick (con cui ci si è
                  connessi precedentemente), msg.data.hdr.receiver è il nick di
                  chi deve ricevere il file, msg.data.hdr.len è la lunghezza
                  del buffer (incluso il terminatore \0), msg.data.buf contiene
                  il nome del file in una stringa C valida. A questo segue
                  l'invio effettivo del file.
 * - GETFILE_OP: msg.hdr.sender deve essere il proprio nick (con cui ci si è
                 connessi precedentemente), msg.data.hdr.len è la lunghezza
                 del buffer (incluso il terminatore \0), msg.data.buf contiene
                 il nome del file in una stringa C valida.
 *
 * Le risposte del server possono essere dei seguenti tipi:
 * - Codice di errore, in quel caso contiene solo l'header
 * - OP_OK, nessun dato (risposta di default)
 * - OP_OK, lista degli utenti connessi: il buffer deve essere una stringa
          formata dai nomi dei client connessi, facendo occupare ad ogni nome
          MAX_NAME_LENGTH + 1 caratteri (non utilizzando quelli dopo \0)
 * - OP_OK, lista dei messaggi: il buffer deve essere un size_t* che contiene il
          di messaggi della history (non importa il valore di msg.data.hdr.len).
          All'invio di questa risposta deve seguire l'invio dei messaggi salvati
          nella history.
 * - OP_OK, file: il buffer contiene il nome del file. A questo messaggio segue
          l'invio effettivo del file.
 *
 * I file vengono inviati come message_data_t che segue l'invio del messaggio
 * vero: data.hdr.receiver è una stringa vuota, data.hdr.len è la lunghezza del
 * file e data.buf è l'intero file.
 *
 * @var message_t::hdr
 * header
 * @var message_t::data
 * body
 */
typedef struct {
    message_hdr_t  hdr;
    message_data_t data;
} message_t;

// ------ funzioni di utilità -------

/**
 * @function setHeader
 * @brief scrive l'header del messaggio
 *
 * @param hdr puntatore all'header
 * @param op tipo di operazione da eseguire
 * @param sender mittente del messaggio
 */
void setHeader(message_hdr_t *hdr, op_t op, char *sender);

/**
 * @function setData
 * @brief scrive la parte dati del messaggio
 *
 * @param data puntatore al body del messaggio
 * @param rcv nickname o groupname del destinatario
 * @param buf puntatore al buffer
 * @param len lunghezza del buffer
 */
void setData(message_data_t *data, char *rcv, const char *buf, unsigned int len);

#ifdef DEBUG
#include <stdio.h>
/**
 * @brief Stampa un messaggio. Funzione esistente solo in DEBUG
 */
void printMsg(message_t msg);
#endif

#endif /* MESSAGE_H_ */
