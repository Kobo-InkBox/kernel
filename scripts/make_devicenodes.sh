#!/bin/sh

if [ -z "$GITDIR" ]; then
	echo "You must specify the GITDIR environment variable."
	exit 1
else
	make_nodes() {
		mkdir -p dev
		sudo mknod dev/console c 5 1
		sudo mknod dev/ttymxc0 c 207 16
		sudo mknod dev/null c 1 3
	}

	cd ${GITDIR}/initrd/n705
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n905c
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n613
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n705-diags
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n905c-diags
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n613-diags
	make_nodes 2>/dev/null
	echo "Done making devices nodes."
fi
