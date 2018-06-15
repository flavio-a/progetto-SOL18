#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>

#include "connections.h"

#define SOCKET_PATH "/tmp/chatty-test-connections.sock"
#define TEST_OP GETPREVMSGS_OP
#define TEST_SENDER "cusu"
#define TEST_RECEIVER "mano"
#define TEST_LEN 150
#define TEST_CONTENT "abcd1abcd2abcd3abcd4abcd5abcd6abcd7abcd8abcd9abcd0abcd1abcd2abcd3abcd4abcd5abcd6abcd7abcd8abcd9abcd0abcd1abcd2abcd3abcd4abcd5abcd6abcd7abcd8abcd9abcd0"

#define SYSCALL(r, c, e) if ((r = c) < 0) { perror(e); exit(errno); }

void myquit(){
	unlink(SOCKET_PATH);
	exit(-1);
}

bool equalData(message_data_t a, message_data_t b) {
	return strlen(a.hdr.receiver) == strlen(b.hdr.receiver)
	       && strncmp(a.hdr.receiver, b.hdr.receiver, strlen(a.hdr.receiver)) == 0
	       && a.hdr.len == b.hdr.len
		   && strncmp(a.buf, b.buf, a.hdr.len) == 0;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "Devi passare 'client' oppure 'server'\n");
		return -1;
	}

	// definisce le strutture dati che il client invia al server
	// il server restituisce errore se le riceve errate
	message_t message;
	setHeader(&(message.hdr), TEST_OP, TEST_SENDER);
	char* buff = malloc(TEST_LEN * sizeof(char));
	for (int i = 0; strlen(TEST_CONTENT) * i < TEST_LEN; ++i)
		strncpy(buff + strlen(TEST_CONTENT) * i, TEST_CONTENT, strlen(TEST_CONTENT));
	setData(&(message.data), TEST_RECEIVER, buff, TEST_LEN);


	if (strncmp(argv[1], "server", 6) == 0) {
		// lato server
		int ssfd = createSocket(SOCKET_PATH);
		int asfd;
		SYSCALL(asfd, accept(ssfd, NULL, 0), "Accepting connection");

		// prova di lettura con readMsg
		message_t reqMsg;
		readMsg(asfd, &reqMsg);
		// confronta il messaggio ricevuto con quello in memoria
		if (reqMsg.hdr.op != message.hdr.op
			|| strncmp(reqMsg.hdr.sender, message.hdr.sender, strlen(message.hdr.sender)) != 0) {
			fprintf(stderr, "Errore nel messaggio: header diversi\n");
			myquit();
		}
		if (!equalData(reqMsg.data, message.data)) {
			fprintf(stderr, "Errore nel messaggio: data diversi\n");
			myquit();
		}

		// prova di lettura con readHeader
		message_hdr_t reqHdr;
		readHeader(asfd, &reqHdr);
		if (reqMsg.hdr.op != message.hdr.op
			|| strncmp(reqMsg.hdr.sender, message.hdr.sender, strlen(message.hdr.sender)) != 0) {
			fprintf(stderr, "Errore nel messaggio: header diversi\n");
			myquit();
		}

		// prova di lettura con readData
		message_data_t reqData;
		readData(asfd, &reqData);
		if (!equalData(reqData, message.data)) {
			fprintf(stderr, "Errore nell'invio dei data: diversi\n");
			myquit();
		}

		// prova di lettura di sendMsg con readHeader e readData successivi
		readHeader(asfd, &reqHdr);
		readData(asfd, &reqData);
		if (reqMsg.hdr.op != message.hdr.op
			|| strncmp(reqMsg.hdr.sender, message.hdr.sender, strlen(message.hdr.sender)) != 0
			|| !equalData(reqData, message.data)) {
			fprintf(stderr, "Errore nel messaggio: header diversi\n");
			myquit();
		}

		// verifica che sul socket non ci sia più niente da leggere
		// Ignora SIGPIPE
		struct sigaction s;
		memset(&s, 0, sizeof(s));
		s.sa_handler = SIG_IGN;
		if (sigaction(SIGPIPE, &s, NULL) < 0) {
			perror("sigaction");
			return -1;
		}
		char buf[1];
		if (read(asfd, buf, 1) != 0) {
			fprintf(stderr, "Errore: socket non vuoto dopo la lettura del messaggio\n");
			myquit();
		}
	}
	else {
		// lato client
		int csfd = openConnection(SOCKET_PATH, 10, 1);
		sendRequest(csfd, &message);
		sendHeader(csfd, &message.hdr);
		sendData(csfd, &message.data);
		sendRequest(csfd, &message);
	}

	unlink(SOCKET_PATH);
	free(buff);

	return 0;
}
