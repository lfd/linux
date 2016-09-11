/*
 * Console driver for running over the Jailhouse partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2016
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/module.h>
#include <linux/console.h>
#ifdef CONFIG_X86
#include <asm/alternative.h>
#endif
#ifdef CONFIG_ARM
#include <asm/opcodes-virt.h>
#endif

#define JAILHOUSE_HC_DEBUG_CONSOLE_PUTC		8

static int hypervisor_putc(char c)
{
	int result;

#ifdef CONFIG_X86
	asm volatile(
		ALTERNATIVE(".byte 0x0f,0x01,0xc1", ".byte 0x0f,0x01,0xd9",
			    X86_FEATURE_VMMCALL)
		: "=a" (result)
		: "a" (JAILHOUSE_HC_DEBUG_CONSOLE_PUTC), "D" (c)
		: "memory");
#endif
#ifdef CONFIG_ARM
	register u32 num_res asm("r0") = JAILHOUSE_HC_DEBUG_CONSOLE_PUTC;
	register u32 arg1 asm("r1") = c;

	asm volatile(
		__HVC(0x4a48)
		: "=r" (num_res)
		: "r" (num_res), "r" (arg1)
		: "memory");
	result = num_res;
#endif

	return result;
}

static void jailhouse_dbgcon_write(struct console *con, const char *s,
				   unsigned count)
{
	while (count > 0) {
		hypervisor_putc(*s);
		count--;
		s++;
	}
}

static struct console jailhouse_dbgcon = {
	.name = "jailhouse",
	.write = jailhouse_dbgcon_write,
	.flags = CON_PRINTBUFFER | CON_ANYTIME,
	.index = -1,
};

static int __init jailhouse_dbgcon_init(void)
{
	register_console(&jailhouse_dbgcon);
	return 0;
}

static void __exit jailhouse_dbgcon_exit(void)
{
	unregister_console(&jailhouse_dbgcon);
}

module_init(jailhouse_dbgcon_init);
module_exit(jailhouse_dbgcon_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Jailhouse debug console driver");
MODULE_AUTHOR("Jan Kiszka <jan.kiszka@siemens.com>");
