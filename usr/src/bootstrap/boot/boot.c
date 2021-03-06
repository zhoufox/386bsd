/*-
 * Copyright (c) 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * $Id: boot.c,v 1.1 94/10/20 16:45:28 root Exp $
 */

#include "sys/param.h"
#include "sys/reboot.h"
#include <a.out.h>
#include "saio.h"
#include "disklabel.h"
#include "ufs_dinode.h"

/*
 * Boot program, loaded by boot block from remaing 7.5K of boot area.
 * Sifts through disklabel and attempts to load an program image of
 * a standalone program off the disk. If keyboard is hit during load,
 * or if an error is encounter, try alternate files.
 */

char *dfiles[] = { ".askname", ".single", ".dfltroot",".kdb",
	 /* ".diagnostic", ".debug", */ 0};
int dflags[] = {RB_ASKNAME, RB_SINGLE, RB_DFLTROOT, RB_KDB};

char *files[] = { "386bsd", "386bsd.alt", "386bsd.old", "boot" , 0};
int	retry = 0;
extern struct disklabel disklabel;
extern	int bootdev, cyloffset;
static unsigned char *biosparams = (char *) 0x9ff00; /* XXX */

/*
 * Boot program... loads /boot out of filesystem indicated by arguements.
 * We assume an autoboot unless we detect a misconfiguration.
 */

main(dev, unit, off)
{
	register struct disklabel *lp;
	register int io;
	register char **bootfile;
	int howto = 0;
	extern int scsisn; /* XXX */


	/* are we a disk, if so look at disklabel and do things */
	lp = &disklabel;

#if	DEBUG > 0
	printf("cyl %x %x hd %x sect %x ",
		biosparams[0], biosparams[1], biosparams[2], biosparams[0xe]);
	printf("dev %x unit %x off %d\n", dev, unit, off);
#endif

	if (lp->d_magic == DISKMAGIC) {
	    /*
	     * Synthesize bootdev from dev, unit, type and partition
	     * information from the block 0 bootstrap.
	     * It's dirty work, but someone's got to do it.
	     * This will be used by the filesystem primatives, and
	     * drivers. Ultimately, opendev will be created corresponding
	     * to which drive to pass to top level bootstrap.
	     */
	    for (io = 0; io < lp->d_npartitions; io++) {
		int sn;

		if (lp->d_partitions[io].p_size == 0)
			continue;
		if (lp->d_type == DTYPE_SCSI)
			sn = off;
		else
			sn = off * lp->d_secpercyl;
		if (lp->d_partitions[io].p_offset == sn)
			break;
	    }

	    if (io == lp->d_npartitions) goto screwed;
            cyloffset = off;
	} else {
screwed:
		/* probably a bad or non-existant disklabel */
		io = 0 ;
		howto |= RB_SINGLE|RB_ASKNAME ;
	}

	/* construct bootdev */
	/* currently, PC has no way of booting off alternate controllers */
	bootdev = MAKEBOOTDEV(/*i_dev*/ dev, /*i_adapt*/0, /*i_ctlr*/0,
	    unit, /*i_part*/io, /*i_fs*/ 1);

	/* first, probe for files associated with special flags quietly */
	bootfile = dfiles;
	for (;;) {
		io = namei(*bootfile);
		if (io > 2) {
			howto |= dflags[bootfile - dfiles];
#if	DEBUG > 0
		printf("howto %x ", howto);
#endif
		}

		if(*++bootfile == 0)
			break;
	}

	bootfile = files;
	for (;;) {
		io = namei(*bootfile);
		if (io > 2)
			copyunix(io, howto, off);
		else
			printf("File not found");

		printf(" - didn't load %s, ", *bootfile);
		if (*++bootfile == 0)
			bootfile = files;
		printf("will try %s\n", *bootfile);

		wait(1<<((retry++) + 10));
	}
}

/*ARGSUSED*/
copyunix(io, howto, cyloff)
	register io;
{
	struct exec x;
	int i;
	char *addr;
	struct dinode fil;
	int off, baseamt, extamt;

	fetchi(io, &fil);
#if	DEBUG > 1
printf("mode %o ", fil.di_mode);
#endif
	i = iread(&fil, 0,  (char *)&x, sizeof x);
	off = sizeof x;
	if (i != sizeof x || x.a_magic != 0413) {
		printf("File is not an executable format");
		return;
	}

	/* check if image loaded will be larger than main memory */
	/* if (roundup(x.a_text, 4096) + x.a_data + x.a_bss > ???) {
		printf("Program larger than memory");
		return;
	} */
	addr = 0;
	off = 4096;

	/* check if program instruction contents larger than "low" RAM" */
	if ((unsigned)addr <= 0x90000 && (unsigned)addr + x.a_text > 0x90000) {
#if	DEBUG > 1
		printf("File text split in two");
#endif
		baseamt = 0x90000 - (unsigned)addr;
	} else
		baseamt = x.a_text;

#if	DEBUG > 1
	printf("o %x a %x s %x ", off, addr, baseamt);
#endif
	if (iread(&fil, off, addr, baseamt) != baseamt)
		goto shread;
	off += baseamt;
	addr += baseamt;

	if (baseamt != x.a_text) {
		addr = (char *)0x100000;
		extamt = x.a_text - baseamt;
#if	DEBUG > 1
		printf("o %x a %x s %x ", off, addr, extamt);
#endif
		if (iread(&fil, off, addr, extamt) != extamt)
			goto shread;
		off += extamt;
		addr += extamt;
	}

	addr = (char *)x.a_text;
	while ((int)addr & (NBPG-1))
		*addr++ = 0;
	
	/* check if program data contents larger than "low" RAM" */
	if ((unsigned)addr <= 0x90000 && (unsigned)addr + x.a_data > 0x90000) {
#if	DEBUG > 1
		printf("File data split in two");
#endif
		baseamt = 0x90000 - (int)addr;
	} else
		baseamt = x.a_data;

#if	DEBUG > 1
	printf("o %x a %x s %x ", off, addr, baseamt);
#endif
	if (iread(&fil, off, addr, baseamt) != baseamt)
		goto shread;
	off += baseamt;
	addr += baseamt;

	if (baseamt != x.a_data) {
		addr = (char *)0x100000;
		extamt = x.a_data - baseamt;
#if	DEBUG > 1
		printf("o %x a %x s %x ", off, addr, extamt);
#endif
		if (iread(&fil, off, addr, extamt) != extamt)
			goto shread;
		off += extamt;
		addr += extamt;
	}

#if	DEBUG > 1
	printf("o %x a %x bss %x ", off, addr, x.a_bss);
#endif
	if ((unsigned)addr <= 0x90000 && (unsigned)addr + x.a_bss > 0x90000) {
#if	DEBUG > 1
		printf("File BSS split in two");
#endif
		baseamt = 0x90000 - (int)addr;
		bzero(addr, baseamt);
		addr = (char *)0x100000;
		extamt = x.a_bss - baseamt;
		bzero(addr, extamt);
		addr += extamt;
	} else {
		bzero(addr, x.a_bss);
		addr += x.a_bss;
	}
/* XXX: read syms and strings */
{
	io = namei(".config");
	if (io > 2) {
printf("cfg ");
/*addr += 4096; (int)addr &= ~(4096-1);*/
		fetchi(io, &fil);
		(void)iread(&fil, 0, addr, 4096);
	}
}

	/* mask high order bits corresponding to relocated system base */
	x.a_entry &= ~0xfff00000;

	/*if (scankbd()) {
		printf("Operator abort");
		kbdreset();
		return;
	}*/

#if	DEBUG > 0
	printf("entry %x [%x]\n", x.a_entry, *(int *) x.a_entry);
#endif
	bcopy(0x9ff00, 0x300, 0x20); /* XXX */
	i = (*((int (*)()) x.a_entry))(howto, bootdev, 0x90000);

	if (i)
		printf("Program exits with %d", i) ; 
	return;
shread:
	printf("Read of program is incomplete");
	return;
}
