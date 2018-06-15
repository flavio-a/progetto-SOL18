#include "message.h"

/**
 * @file  message.c
 * @brief Implementazione di message.h
 */

 // La documentazione dei metodi pubblici di questo file è in connections.h

// scrive l'header del messaggio
void setHeader(message_hdr_t *hdr, op_t op, char *sender) {
#if defined(MAKE_VALGRIND_HAPPY)
    memset((char*)hdr, 0, sizeof(message_hdr_t));
#endif
    hdr->op  = op;
    strncpy(hdr->sender, sender, strlen(sender)+1);
}

//scrive la parte dati del messaggio
void setData(message_data_t *data, char *rcv, const char *buf, unsigned int len) {
#if defined(MAKE_VALGRIND_HAPPY)
    memset((char*)&(data->hdr), 0, sizeof(message_data_hdr_t));
#endif

    strncpy(data->hdr.receiver, rcv, strlen(rcv)+1);
    data->hdr.len  = len;
    data->buf      = (char *)buf;
}

#ifdef DEBUG

void printMsg(message_t msg) {
    fprintf(stderr, "==== Messaggio ====\n");
    fprintf(stderr, "Hdr:\n op: %d\n sender: \"%s\"\n", msg.hdr.op, msg.hdr.sender);
    fprintf(stderr, "Data hdr:\n len: %d\n receiver: \"%s\"\n", msg.data.hdr.len, msg.data.hdr.receiver);
}

#endif
