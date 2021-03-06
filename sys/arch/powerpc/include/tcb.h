/*	$OpenBSD: tcb.h,v 1.5 2016/09/04 08:42:47 guenther Exp $	*/

/*
 * Copyright (c) 2011 Philip Guenther <guenther@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _MACHINE_TCB_H_
#define _MACHINE_TCB_H_

#ifdef _KERNEL

#define TCB_GET(p)		\
	((void *)trapframe(p)->fixreg[2])
#define TCB_SET(p, addr)	\
	(trapframe(p)->fixreg[2] = (__register_t)(addr))

#else /* _KERNEL */

/* ELF TLS ABI calls for small TCB, with static TLS data after it */
#define TLS_VARIANT	1

/* powerpc offsets the TCB pointer 0x7000 bytes after the data */
#define TCB_OFFSET	0x7000

register void *__tcb __asm__ ("%r2");
#define TCB_GET()		(__tcb)
#define TCB_SET(tcb)		((__tcb) = (tcb))

#endif /* _KERNEL */
#endif /* _MACHINE_TCB_H_ */
