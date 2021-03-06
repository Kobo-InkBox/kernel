#!/bin/sh

if [ -z "${GITDIR}" ]; then
	echo "You must specify the GITDIR environment variable."
	exit 1
else
	make_nodes() {
		mkdir -p dev
		sudo mknod dev/console c 5 1
		sudo mknod dev/null c 1 3
		if [ "${1}" == "emu" ]; then
			sudo mknod dev/ttyAMA0 c 204 64
		elif [ "${1}" == "bpi" ]; then
			sudo mknod dev/ttyS0 c 4 64
		else
			sudo mknod dev/ttymxc0 c 207 16
		fi
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
	cd ${GITDIR}/initrd/n873-spl
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n873
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n873-diags
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n905b
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n905b-diags
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n236
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n437
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n306
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/n249
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/kt
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/kt-diags
	make_nodes 2>/dev/null
	cd ${GITDIR}/initrd/emu
	make_nodes emu 2>/dev/null
	cd ${GITDIR}/initrd/emu-diags
	make_nodes emu 2>/dev/null
	cd ${GITDIR}/initrd/bpi
	make_nodes bpi 2>/dev/null
	cd ${GITDIR}/initrd/bpi-diags
	make_nodes bpi 2>/dev/null
	echo "Done making devices nodes."
fi
