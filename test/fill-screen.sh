#! /bin/bash

COUNT=30
for i in `seq 1 ${COUNT}`; do
	echo -ne "$i ..."
	if [[ $i -ne ${COUNT} ]]; then
		echo
	else
		echo -n " > "
	fi
done
