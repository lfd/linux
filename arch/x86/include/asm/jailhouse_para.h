/*
 * Jailhouse paravirt_ops implementation
 *
 * Copyright (c) Siemens AG, 2015
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef _ASM_X86_JAILHOUSE_PARA_H
#define _ASM_X86_JAILHOUSE_PARA_H

#include <linux/types.h>

#ifdef CONFIG_JAILHOUSE_GUEST
bool jailhouse_paravirt(void);
#else
static inline bool jailhouse_paravirt(void)
{
	return false;
}
#endif

#endif /* _ASM_X86_JAILHOUSE_PARA_H */
