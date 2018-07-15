#
# chatterbox Progetto del corso di LSO 2017/2018
#
# Dipartimento di Informatica Università di Pisa
# Docenti: Prencipe, Torquati
#
#

##########################################################
# IMPORTANTE: completare la lista dei file da consegnare
#
FILE_DA_CONSEGNARE=Makefile chatty.c message.h ops.h stats.h config.h \
           DATA/chatty.conf1 DATA/chatty.conf2 connections.h \
           message.c lock.h lock.c fifo.h fifo.c icl_hash.h icl_hash.c \
           hashtable.h hashtable.c nickname.h nickname.c connections.c \
		   testconnections.c testfifo.c testhashtable.c testicl_hash.c \
		   relazione/relazione.pdf
# inserire il nome del tarball: es. NinoBixio
TARNAME=FlavioAscari
# inserire il corso di appartenenza: CorsoA oppure CorsoB
CORSO=CorsoB
#
# inserire l'email sulla quale ricevere comunicazione sul progetto
# e per sostenere l'esame
MAIL=flavio.ascari@sns.it
#
###########################################################

###################################################################
# NOTA: Il nome riportato in UNIX_PATH deve corrispondere al nome
#       usato per l'opzione UnixPath nel file di configurazione del
#       server (vedere i file nella directory DATA).
#       Lo stesso vale per il nome riportato in STAT_PATH e DIR_PATH
#       che deveno corrispondere con l'opzione StatFileName e
#       DirName, rispettivamente.
#
# ATTENZIONE: se il codice viene sviluppato sulle macchine del
#             laboratorio utilizzare come nomi, nomi unici,
#             ad esempo /tmp/chatty_sock_<numero-di-matricola> e
#             /tmp/chatty_stats_<numero-di-matricola>.
#
###################################################################
UNIX_PATH       = /tmp/chatty_socket
STAT_PATH       = /tmp/chatty_stats.txt
DIR_PATH        = /tmp/chatty

CC			=  gcc
AR			=  ar
CFLAGS		+= -std=c99 -Wall -pedantic -g -DMAKE_VALGRIND_HAPPY
ARFLAGS		=  rvs
INCLUDES	= -I.
LDFLAGS 	= -L.
OPTFLAGS	= -O2
LIBS		= -pthread

# commentare questa riga per evitare il debug
# CFLAGS		+= -DDEBUG
# commentare questa riga per evitare il debug verboso
# CFLAGS		+= -DVERBOSE

# aggiungere qui altri targets se necessario
TARGETS		= chatty \
			  client


# aggiungere qui i file oggetto da compilare
OBJECTS		= connections.o \
			  message.o \
			  lock.o \
			  fifo.o \
			  icl_hash.o \
			  hashtable.o \
			  nickname.o \
			  worker.o

# aggiungere qui gli altri include
INCLUDE_FILES = connections.h \
				message.h \
				ops.h \
				stats.h \
				config.h \
				lock.h \
				fifo.h \
				icl_hash.h \
				hashtable.h \
				nickname.h \
				worker.h

.PHONY: all clean cleanall doc test1 test2 test3 test4 test5 consegna
.SUFFIXES: .c .h

%: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(OPTFLAGS) $(LDFLAGS) -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) $(OPTFLAGS) -c -o $@ $<

all: $(TARGETS)

chatty: chatty.o libchatty.a $(INCLUDE_FILES)
	$(CC) $(CFLAGS) $(INCLUDES) $(OPTFLAGS) $(LDFLAGS) $(LIBS) -o $@ $^

client: client.o connections.c message.c
	$(CC) $(CFLAGS) $(INCLUDES) $(OPTFLAGS) $(LDFLAGS) $(LIBS) -o $@ $^

doc:
	doxygen


########################### makerules per eseguire i test intermedi

TESTS = connections fifo hashtable icl_hash

SPECIAL_TESTS = connections

.PHONY: cleantest $(addprefix runtest, $(TESTS))

# si potrebbe evitare l'addprefix iniziale, ma così la shell autocompleta
$(addprefix test, $(TESTS)): test%: test%.c libchatty.a $(INCLUDE_FILES)
	$(CC) $(CFLAGS) $(INCLUDES) $(OPTFLAGS) $(LDFLAGS) $(LIBS) -o $@ $^

$(addprefix runtest, $(filter-out $(SPECIAL_TESTS),$(TESTS))): runtest%: test%
	./$<
	@echo "********** Test superato"

runtestconnections: testconnections
	(./$< server ; echo $$?) &
	sleep 1
	./$< client
	@echo "********** Test superato"


cleantest:
	rm -f $(addprefix test, $(TESTS))

############################ non modificare da qui in poi

libchatty.a: $(OBJECTS)
	$(AR) $(ARFLAGS) $@ $^

clean		:
	rm -f $(TARGETS)

cleanall	: clean
	\rm -f *.o *~ libchatty.a valgrind_out $(STAT_PATH) $(UNIX_PATH)
	\rm -fr  $(DIR_PATH)

killchatty:
	killall -9 chatty

# test base
test1:
	make cleanall
	\mkdir -p $(DIR_PATH)
	make all
	./chatty -f DATA/chatty.conf1&
	./client -l $(UNIX_PATH) -c pippo
	./client -l $(UNIX_PATH) -c pluto
	./client -l $(UNIX_PATH) -c minni
	./client -l $(UNIX_PATH) -k pippo -S "Ciao pluto":pluto -S "come stai?":pluto
	./client -l $(UNIX_PATH) -k pluto -p -S "Ciao pippo":pippo -S "bene e tu?":pippo -S "Ciao minni come stai?":minni
	./client -l $(UNIX_PATH) -k pippo -p
	./client -l $(UNIX_PATH) -k pluto -p
	./client -l $(UNIX_PATH) -k minni -p
	killall -QUIT -w chatty
	@echo "********** Test1 superato!"

# test scambio file
test2:
	make cleanall
	\mkdir -p $(DIR_PATH)
	make all
	./chatty -f DATA/chatty.conf1&
	./testfile.sh $(UNIX_PATH) $(DIR_PATH)
	killall -QUIT -w chatty
	@echo "********** Test2 superato!"

# test parametri di configurazione e statistiche
test3:
	make cleanall
	\mkdir -p $(DIR_PATH)
	make all
	./chatty -f DATA/chatty.conf2&
	./testconf.sh $(UNIX_PATH) $(STAT_PATH)
	killall -QUIT -w chatty
	@echo "********** Test3 superato!"


# verifica di memory leaks
test4:
	make cleanall
	\mkdir -p $(DIR_PATH)
	make all
	./testleaks.sh $(UNIX_PATH)
	@echo "********** Test4 superato!"

# stress test
test5:
	make cleanall
	\mkdir -p $(DIR_PATH)
	make all
	./chatty -f DATA/chatty.conf1&
	./teststress.sh $(UNIX_PATH)
	killall -QUIT -w chatty
	@echo "********** Test5 superato!"

# target per la consegna
consegna:
	make test1
	sleep 3
	make test2
	sleep 3
	make test3
	sleep 3
	make test4
	sleep 3
	make test5
	sleep 3
	tar -cvf $(TARNAME)_$(CORSO)_chatty.tar $(FILE_DA_CONSEGNARE)
	@echo "*** TAR PRONTO $(TARNAME)_$(CORSO)_chatty.tar "
	@echo "Per la consegna seguire le istruzioni specificate nella pagina del progetto:"
	@echo " http://didawiki.di.unipi.it/doku.php/informatica/sol/laboratorio17/progetto"
	@echo
