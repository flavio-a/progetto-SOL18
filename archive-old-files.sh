#!/bin/bash

TMPARCHIVE="/tmp/chatty-tmp-archive/"

if [[ -z $1 ]] || [[ $# == "--help" ]] || [[ $# == "-h" ]]; then
	echo 'Lo script si usa così:'
	echo $0 conf_file t
	echo ' - conf_file è il file di configurazione di chatty'
	echo ' - t è la minima età (in minuti) dei file che vengono archiviati'
	exit 1
fi

DIRNAME=$(grep -oP 'DirName\s*=\s*\K[\w\s\-\/]+' $1)
echo $DIRNAME

if [[ $2 == 0 ]]; then
	find -P "${DIRNAME}/" -print
else
	mkdir $TMPARCHIVE
	find -P "${DIRNAME}" -mindepth 1 -mmin +$2 -execdir mv {} $TMPARCHIVE \; -execdir echo {} \;
	tar -C $TMPARCHIVE/ -czvf chatty-oldfiles.tar.gz .
	if [[ $? != 0 ]]; then
		mv $TMPARCHIVE/* "${DIRNAME}/"
	fi
	rm -rf $TMPARCHIVE
fi
