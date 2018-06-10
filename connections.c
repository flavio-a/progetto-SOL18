#include "connections.h"

/**
 * @file  connections.c
 * @brief Implementazione di connections.h
 */

// La documentazione dei metodi pubblici di questo file è in connections.h

// -------- connection handlers --------

// Crea il socket lato server
int createSocket(char* path) {
	// create server socket
	int ssfd;
	struct sockaddr_un sa;
	strncpy(sa.sun_path, path, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	if ((ssfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("Creating socket");
		return -1;
	}
	if (bind(ssfd, (struct sockaddr*) &sa, sizeof(sa)) < 0) {
		perror("Binding socket");
		return -1;
	}
	if (listen(ssfd, SOMAXCONN) < 0) {
		perror("Listening on socket");
		return -1;
	}
	return ssfd;
}

// Apre una connessione AF_UNIX verso il server
int openConnection(char* path, unsigned int ntimes, unsigned int secs) {
	if (ntimes > MAX_RETRIES) {
		fprintf(stderr, "Requested more than %d retries, setting to %d",
		        MAX_RETRIES, MAX_RETRIES);
		ntimes = MAX_RETRIES;
	}
	if (secs > MAX_SLEEPING) {
		fprintf(stderr, "Requested more than %d seconds between retries, setting to %d",
		        MAX_SLEEPING, MAX_SLEEPING);
		secs = MAX_SLEEPING;
	}
	// create client socket
	int csfd;
	struct sockaddr_un sa;
	strncpy(sa.sun_path, path, UNIX_PATH_MAX);
	sa.sun_family = AF_UNIX;
	if ((csfd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		perror("Creating socket");
		return -1;
	}
	while (connect(csfd, (struct sockaddr*) &sa, sizeof(sa)) < 0 && ntimes-- > 0) {
		fprintf(stderr, "Connection failed, retrying...\n");
		sleep(secs);
	}
	if (ntimes == 0) {
		perror("Connecting socket");
		return -1;
	}
	return csfd;
}


// -------- receiver side -----

/**
* @brief Legge un certo numero di byte da un file descriptor
*
* La lettura avviene garantendo la sicurezza da interruzioni (la read viene
* riavviata) o da short read/write.
* Oltre a leggere i byte, imposta errno se la read restituisce un errore
*
* @param fd il descrittore di file da cui leggere
* @param buf il puntatore su cui scrivere i dati letti.
*            Si aspetta che punti ad una locazione che può
*            contenere almeno size byte
* @param byte la quantità di byte da leggere
*
* @return 1 se ha letto tutto il messaggio,
*         0 se ha letto 0 byte (ovvero se la connessione è chiusa),
*         < 0 in caso di errore (e imposta errno)
*/
static int readByte(long fd, void* buf, size_t byte) {
	ssize_t byte_read;
	while (byte > 0) {
		byte_read = read(fd, buf, byte);
		if (byte_read < 0) {
			if (errno == EINTR)
				continue; //continua il ciclo
			// vuol dire che il socket ha avuto dei problemi
			errno = EPIPE;
			return -1;
		}
		if (byte_read == 0)
			return 0;
		byte -= byte_read;
		buf = (void*)((char *)buf + byte_read); // per incrementare buf di
		                                        // byte_read byte
	}
	return 1; // se arriva qua ha letto tutto il messaggio correttamente
}

// Legge l'header del messaggio
int readHeader(long fd, message_hdr_t *hdr) {
	#if defined(MAKE_VALGRIND_HAPPY)
	    memset(hdr, 0, sizeof(message_hdr_t));
	#endif
	return readByte(fd, hdr, sizeof(message_hdr_t));
	// readByte restituisce già il valore corretto, impostando errno se serve
}

// Legge il body del messaggio
int readData(long fd, message_data_t *data) {
	#if defined(MAKE_VALGRIND_HAPPY)
	    memset(data, 0, sizeof(message_data_t));
	#endif
	// Legge l'header dei dati
	int result = readByte(fd, &(data->hdr), sizeof(message_data_hdr_t));
	if (result <= 0)
		return result; //errno già impostato da readByte

	// Legge i dati veri e propri
	data->buf = malloc(data->hdr.len);
	return readByte(fd, data->buf, data->hdr.len);
	// valore di ritorno di readByte già corretto
	// errno già impostato da readByte
}

// Legge l'intero messaggio
int readMsg(long fd, message_t *msg) {
	int result = readHeader(fd, &(msg->hdr));
	if (result <= 0)
		return result;
	return readData(fd, &(msg->data));
	// readData restituisce già il valore corretto, impostando errno se serve
}


// ------- sender side ------

/**
* @brief Scrive un certo numero di byte su un file descriptor
*
* La scrittura avviene garantendo la sicurezza da interruzioni (la write viene
* riavviata) o da short read/write.
* Oltre a scrivere i byte, imposta errno se la write restituisce un errore
*
* @param fd il descrittore di file su cui scrivere
* @param buf il puntatore da cui leggere i dati scritti.
* @param byte la quantità di byte da scrivere
*
* @return 1 se ha scritto tutto il messaggio,
*         0 se ha scritto 0 byte (ovvero se la connessione è chiusa),
*         < 0 in caso di errore (e imposta errno)
 */
static int sendByte(long fd, void* buf, size_t byte) {
	ssize_t byte_written;
	while (byte > 0) {
		byte_written = write(fd, buf, byte);
		if (byte_written < 0) {
			if (errno == EINTR)
				continue; // continua il ciclo
			errno = EPIPE;
			return -1;
		}
		if (byte_written == 0)
			return 0;
		byte -= byte_written;
		buf = (void*)((char *)buf + byte_written); // per incrementare buf di
		                                           // byte_read byte
	}
	return 1;
}


// Invia un messaggio di rischiesta al server
int sendRequest(long fd, message_t *msg) {
	// Scrive l'header
	int result = sendByte(fd, &(msg->hdr), sizeof(message_hdr_t));
	if (result <= 0)
		return result;  //errno già impostato da readByte
	if (msg->data == NULL)
		return result;
	return sendData(fd, &(msg->data));
}

// Invia il body di un messaggio al server
int sendData(long fd, message_data_t *data) {
	// Scrive l'header dei dati
	int result = sendByte(fd, &(data->hdr), sizeof(message_data_hdr_t));
	if (result <= 0)
		return result; //errno già impostato da readByte

	// Scriver i dati veri e propri
	return sendByte(fd, data->buf, data->hdr.len);
	// valore di ritorno di sendByte già corretto
	// errno già impostato da sendByte
}
