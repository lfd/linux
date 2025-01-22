// SPDX-License-Identifier: GPL-2.0
/*
 *  Driver for GRLIB serial ports (APBUART)
 *
 *  Based on linux/drivers/serial/amba.c
 *
 *  Copyright (C) 2000 Deep Blue Solutions Ltd.
 *  Copyright (C) 2003 Konrad Eisele <eiselekd@web.de>
 *  Copyright (C) 2006 Daniel Hellstrom <daniel@gaisler.com>, Aeroflex Gaisler AB
 *  Copyright (C) 2008 Gilead Kutnick <kutnickg@zin-tech.com>
 *  Copyright (C) 2009 Kristoffer Glembo <kristoffer@gaisler.com>, Aeroflex Gaisler AB
 */

#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/serial.h>
#include <linux/console.h>
#include <linux/sysrq.h>
#include <linux/kthread.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <linux/clk.h>
#include <asm/irq.h>

#include "apbuart.h"

#define SERIAL_APBUART_MAJOR	TTY_MAJOR
#define SERIAL_APBUART_MINOR	64
#define UART_DUMMY_RSR_RX	0x8000	/* for ignore all read */

static void apbuart_tx_chars(struct uart_port *port);

static void apbuart_stop_tx(struct uart_port *port)
{
	unsigned int cr;

	cr = UART_GET_CTRL(port);
	cr &= ~UART_CTRL_TI;
	UART_PUT_CTRL(port, cr);
}

static void apbuart_start_tx(struct uart_port *port)
{
	unsigned int cr;

	cr = UART_GET_CTRL(port);
	cr |= UART_CTRL_TI;
	UART_PUT_CTRL(port, cr);

	if (UART_GET_STATUS(port) & UART_STATUS_THE)
		apbuart_tx_chars(port);
}

static void apbuart_stop_rx(struct uart_port *port)
{
	unsigned int cr;

	cr = UART_GET_CTRL(port);
	cr &= ~(UART_CTRL_RI);
	UART_PUT_CTRL(port, cr);
}

static void apbuart_rx_chars(struct uart_port *port)
{
	unsigned int status, rsr;
	unsigned int max_chars = port->fifosize;
	u8 ch, flag;

	status = UART_GET_STATUS(port);

	while (UART_RX_DATA(status) && (max_chars--)) {

		ch = UART_GET_CHAR(port);
		flag = TTY_NORMAL;

		port->icount.rx++;

		rsr = UART_GET_STATUS(port) | UART_DUMMY_RSR_RX;
		UART_PUT_STATUS(port, 0);
		if (rsr & UART_STATUS_ERR) {

			if (rsr & UART_STATUS_BR) {
				rsr &= ~(UART_STATUS_FE | UART_STATUS_PE);
				port->icount.brk++;
				if (uart_handle_break(port))
					goto ignore_char;
			} else if (rsr & UART_STATUS_PE) {
				port->icount.parity++;
			} else if (rsr & UART_STATUS_FE) {
				port->icount.frame++;
			}
			if (rsr & UART_STATUS_OE)
				port->icount.overrun++;

			rsr &= port->read_status_mask;

			if (rsr & UART_STATUS_PE)
				flag = TTY_PARITY;
			else if (rsr & UART_STATUS_FE)
				flag = TTY_FRAME;
		}

		if (uart_handle_sysrq_char(port, ch))
			goto ignore_char;

		uart_insert_char(port, rsr, UART_STATUS_OE, ch, flag);


	      ignore_char:
		status = UART_GET_STATUS(port);
	}

	tty_flip_buffer_push(&port->state->port);
}

static void apbuart_tx_chars(struct uart_port *port)
{
	u8 ch;

	uart_port_tx_limited(port, ch, port->fifosize,
		true,
		UART_PUT_CHAR(port, ch),
		({}));
}

static irqreturn_t apbuart_int(int irq, void *dev_id)
{
	struct uart_port *port = dev_id;
	unsigned int status;

	uart_port_lock(port);

	status = UART_GET_STATUS(port);
	if (status & UART_STATUS_DR)
		apbuart_rx_chars(port);
	if (status & UART_STATUS_THE)
		apbuart_tx_chars(port);

	uart_port_unlock(port);

	return IRQ_HANDLED;
}

static unsigned int apbuart_tx_empty(struct uart_port *port)
{
	unsigned int status = UART_GET_STATUS(port);
	return status & UART_STATUS_THE ? TIOCSER_TEMT : 0;
}

static unsigned int apbuart_get_mctrl(struct uart_port *port)
{
	/* The GRLIB APBUART handles flow control in hardware */
	return TIOCM_CAR | TIOCM_DSR | TIOCM_CTS;
}

static void apbuart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
	/* The GRLIB APBUART handles flow control in hardware */
}

static void apbuart_break_ctl(struct uart_port *port, int break_state)
{
	/* We don't support sending break */
}

static int apbuart_startup(struct uart_port *port)
{
	int retval;
	unsigned int cr;

	/* Allocate the IRQ */
	retval = request_irq(port->irq, apbuart_int, 0, "apbuart", port);
	if (retval)
		return retval;

	/* Finally, enable interrupts */
	cr = UART_GET_CTRL(port);
	UART_PUT_CTRL(port,
		      cr | UART_CTRL_RE | UART_CTRL_TE |
		      UART_CTRL_RI | UART_CTRL_TI);

	return 0;
}

static void apbuart_shutdown(struct uart_port *port)
{
	unsigned int cr;

	/* disable all interrupts, disable the port */
	cr = UART_GET_CTRL(port);
	UART_PUT_CTRL(port,
		      cr & ~(UART_CTRL_RE | UART_CTRL_TE |
			     UART_CTRL_RI | UART_CTRL_TI));

	/* Free the interrupt */
	free_irq(port->irq, port);
}

static void apbuart_set_termios(struct uart_port *port,
				struct ktermios *termios, const struct ktermios *old)
{
	unsigned int cr;
	unsigned long flags;
	unsigned int baud, quot;

	/* Ask the core to calculate the divisor for us. */
	baud = uart_get_baud_rate(port, termios, old, 0, port->uartclk / 16);
	if (baud == 0)
		panic("invalid baudrate %i\n", port->uartclk / 16);

	/* uart_get_divisor calc a *16 uart freq, apbuart is *8 */
	quot = (uart_get_divisor(port, baud)) * 2;
	cr = UART_GET_CTRL(port);
	cr &= ~(UART_CTRL_PE | UART_CTRL_PS);

	if (termios->c_cflag & PARENB) {
		cr |= UART_CTRL_PE;
		if ((termios->c_cflag & PARODD))
			cr |= UART_CTRL_PS;
	}

	/* Enable flow control. */
	if (termios->c_cflag & CRTSCTS)
		cr |= UART_CTRL_FL;

	uart_port_lock_irqsave(port, &flags);

	/* Update the per-port timeout. */
	uart_update_timeout(port, termios->c_cflag, baud);

	port->read_status_mask = UART_STATUS_OE;
	if (termios->c_iflag & INPCK)
		port->read_status_mask |= UART_STATUS_FE | UART_STATUS_PE;

	/* Characters to ignore */
	port->ignore_status_mask = 0;
	if (termios->c_iflag & IGNPAR)
		port->ignore_status_mask |= UART_STATUS_FE | UART_STATUS_PE;

	/* Ignore all characters if CREAD is not set. */
	if ((termios->c_cflag & CREAD) == 0)
		port->ignore_status_mask |= UART_DUMMY_RSR_RX;

	/* Set baud rate */
	quot -= 1;
	UART_PUT_SCAL(port, quot);
	UART_PUT_CTRL(port, cr);

	uart_port_unlock_irqrestore(port, flags);
}

static const char *apbuart_type(struct uart_port *port)
{
	return port->type == PORT_APBUART ? "GRLIB/APBUART" : NULL;
}

static void apbuart_release_port(struct uart_port *port)
{
	release_mem_region(port->mapbase, 0x100);
}

static int apbuart_request_port(struct uart_port *port)
{
	return request_mem_region(port->mapbase, 0x100, "grlib-apbuart")
	    != NULL ? 0 : -EBUSY;
	return 0;
}

/* Configure/autoconfigure the port */
static void apbuart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE) {
		port->type = PORT_APBUART;
		apbuart_request_port(port);
	}
}

/* Verify the new serial_struct (for TIOCSSERIAL) */
static int apbuart_verify_port(struct uart_port *port,
			       struct serial_struct *ser)
{
	int ret = 0;
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_APBUART)
		ret = -EINVAL;
	if (ser->irq < 0 || ser->irq >= NR_IRQS)
		ret = -EINVAL;
	if (ser->baud_base < 9600)
		ret = -EINVAL;
	return ret;
}

static const struct uart_ops grlib_apbuart_ops = {
	.tx_empty = apbuart_tx_empty,
	.set_mctrl = apbuart_set_mctrl,
	.get_mctrl = apbuart_get_mctrl,
	.stop_tx = apbuart_stop_tx,
	.start_tx = apbuart_start_tx,
	.stop_rx = apbuart_stop_rx,
	.break_ctl = apbuart_break_ctl,
	.startup = apbuart_startup,
	.shutdown = apbuart_shutdown,
	.set_termios = apbuart_set_termios,
	.type = apbuart_type,
	.release_port = apbuart_release_port,
	.request_port = apbuart_request_port,
	.config_port = apbuart_config_port,
	.verify_port = apbuart_verify_port,
};

static struct uart_port grlib_apbuart_ports[UART_NR];
static DECLARE_BITMAP(apbuart_ports_in_use, UART_NR);

static int apbuart_scan_fifo_size(struct uart_port *port, int portnumber)
{
	int ctrl, loop = 0;
	int status;
	int fifosize;
	unsigned long flags;

	ctrl = UART_GET_CTRL(port);

	/*
	 * Enable the transceiver and wait for it to be ready to send data.
	 * Clear interrupts so that this process will not be externally
	 * interrupted in the middle (which can cause the transceiver to
	 * drain prematurely).
	 */

	local_irq_save(flags);

	UART_PUT_CTRL(port, ctrl | UART_CTRL_TE);

	while (!UART_TX_READY(UART_GET_STATUS(port)))
		loop++;

	/*
	 * Disable the transceiver so data isn't actually sent during the
	 * actual test.
	 */

	UART_PUT_CTRL(port, ctrl & ~(UART_CTRL_TE));

	fifosize = 1;
	UART_PUT_CHAR(port, 0);

	/*
	 * So long as transmitting a character increments the tranceivier FIFO
	 * length the FIFO must be at least that big. These bytes will
	 * automatically drain off of the FIFO.
	 */

	status = UART_GET_STATUS(port);
	while (((status >> 20) & 0x3F) == fifosize) {
		fifosize++;
		UART_PUT_CHAR(port, 0);
		status = UART_GET_STATUS(port);
	}

	fifosize--;

	UART_PUT_CTRL(port, ctrl);
	local_irq_restore(flags);

	if (fifosize == 0)
		fifosize = 1;

	return fifosize;
}

static void apbuart_flush_fifo(struct uart_port *port)
{
	int i;

	for (i = 0; i < port->fifosize; i++)
		UART_GET_CHAR(port);
}


/* ======================================================================== */
/* Console driver, if enabled                                               */
/* ======================================================================== */

#ifdef CONFIG_SERIAL_GRLIB_GAISLER_APBUART_CONSOLE

static void apbuart_console_putchar(struct uart_port *port, unsigned char ch)
{
	unsigned int status;
	do {
		status = UART_GET_STATUS(port);
	} while (!UART_TX_READY(status));
	UART_PUT_CHAR(port, ch);
}

static void
apbuart_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_port *port = &grlib_apbuart_ports[co->index];
	unsigned int status, old_cr, new_cr;

	/* First save the CR then disable the interrupts */
	old_cr = UART_GET_CTRL(port);
	new_cr = old_cr & ~(UART_CTRL_RI | UART_CTRL_TI);
	UART_PUT_CTRL(port, new_cr);

	uart_console_write(port, s, count, apbuart_console_putchar);

	/*
	 *      Finally, wait for transmitter to become empty
	 *      and restore the TCR
	 */
	do {
		status = UART_GET_STATUS(port);
	} while (!UART_TX_READY(status));
	UART_PUT_CTRL(port, old_cr);
}

static void __init
apbuart_console_get_options(struct uart_port *port, int *baud,
			    int *parity, int *bits)
{
	if (UART_GET_CTRL(port) & (UART_CTRL_RE | UART_CTRL_TE)) {

		unsigned int quot, status;
		status = UART_GET_STATUS(port);

		*parity = 'n';
		if (status & UART_CTRL_PE) {
			if ((status & UART_CTRL_PS) == 0)
				*parity = 'e';
			else
				*parity = 'o';
		}

		*bits = 8;
		quot = UART_GET_SCAL(port) / 8;
		*baud = port->uartclk / (16 * (quot + 1));
	}
}

static int __init apbuart_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	int baud = 38400;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	pr_debug("apbuart_console_setup co=%p, co->index=%i, options=%s\n",
		 co, co->index, options);

	/*
	 * Check whether an invalid uart number has been specified, and
	 * if so, search for the first available port that does have
	 * console support.
	 */
	if (co->index >= UART_NR)
		co->index = 0;

	port = &grlib_apbuart_ports[co->index];

	if (!port->membase)
		return -ENODEV;

	spin_lock_init(&port->lock);

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);
	else
		apbuart_console_get_options(port, &baud, &parity, &bits);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct uart_driver grlib_apbuart_driver;

static struct console grlib_apbuart_console = {
	.name = "ttyGR",
	.write = apbuart_console_write,
	.device = uart_console_device,
	.setup = apbuart_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,
	.data = &grlib_apbuart_driver,
};

static int __init apbuart_console_init(void)
{
	register_console(&grlib_apbuart_console);
	return 0;
}

console_initcall(apbuart_console_init);

#define APBUART_CONSOLE	(&grlib_apbuart_console)
#else
#define APBUART_CONSOLE	NULL
#endif

static struct uart_driver grlib_apbuart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "gaisler-serial",
	.dev_name = "ttyGR",
	.nr = UART_NR,
	.cons = APBUART_CONSOLE,
};


/* ======================================================================== */
/* OF Platform Driver                                                       */
/* ======================================================================== */

static int apbuart_probe(struct platform_device *op)
{
	struct uart_port *port = NULL;
	const int *ampopts;
	struct clk *clk;
	u32 freq_hz = 0;
	struct resource *mem;
	void __iomem *base;
	int line;
	int irq;
	struct device_node *np = op->dev.of_node;

	ampopts = of_get_property(np, "ampopts", NULL);
	if (ampopts && (*ampopts == 0))
		return -ENODEV; /* Ignore if used by another OS instance */

	irq = platform_get_irq(op, 0);
	if (irq < 0)
		return -EPROBE_DEFER;

	mem = platform_get_resource(op, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&op->dev, mem);
	if (IS_ERR(base)) {
		dev_err(&op->dev, "could not acquire device memory\n");
		return PTR_ERR(base);
	}

	of_property_read_u32(np, "freq", &freq_hz);

	if (!freq_hz) {
		clk = devm_clk_get(&op->dev, NULL);
		if (IS_ERR(clk)) {
			dev_err(&op->dev, "unable to find controller clock\n");
			return PTR_ERR(clk);
		}
		freq_hz = clk_get_rate(clk);
	}

	if (!freq_hz)
		return -ENODEV;

	line = of_alias_get_id(np, "serial");
	if (line < 0)
		line = find_first_zero_bit(apbuart_ports_in_use, UART_NR);

	if (line >= UART_NR)
		return -ENODEV;

	if (test_and_set_bit(line, apbuart_ports_in_use))
		return -EBUSY;

	port = &grlib_apbuart_ports[line];

	port->mapbase = mem->start;
	port->membase = base;
	port->irq = irq;
	port->iotype = UPIO_MEM;
	port->ops = &grlib_apbuart_ops;
	port->has_sysrq = IS_ENABLED(CONFIG_SERIAL_GRLIB_GAISLER_APBUART_CONSOLE);
	port->flags = UPF_BOOT_AUTOCONF;
	port->line = line;
	port->uartclk = freq_hz;
	port->fifosize = apbuart_scan_fifo_size((struct uart_port *) port, line);
	port->dev = &op->dev;

	uart_add_one_port(&grlib_apbuart_driver, (struct uart_port *) port);

	apbuart_flush_fifo((struct uart_port *) port);

	printk(KERN_INFO "grlib-apbuart at 0x%llx, irq %d\n",
	       (unsigned long long) port->mapbase, port->irq);
	return 0;
}

static const struct of_device_id apbuart_match[] = {
	{
	 .name = "GAISLER_APBUART",
	 },
	{
	 .name = "01_00c",
	 },
	{
	 .compatible = "gaisler,apbuart",
	 },
	{},
};
MODULE_DEVICE_TABLE(of, apbuart_match);

static struct platform_driver grlib_apbuart_of_driver = {
	.probe = apbuart_probe,
	.driver = {
		.name = "grlib-apbuart",
		.of_match_table = apbuart_match,
	},
};

static int __init grlib_apbuart_init(void)
{
	int ret;

	printk(KERN_INFO "Serial: GRLIB APBUART driver\n");

	ret = uart_register_driver(&grlib_apbuart_driver);
	if (ret) {
		printk(KERN_ERR "%s: uart_register_driver failed (%i)\n",
		       __FILE__, ret);
		return ret;
	}

	ret = platform_driver_register(&grlib_apbuart_of_driver);
	if (ret) {
		printk(KERN_ERR
		       "%s: platform_driver_register failed (%i)\n",
		       __FILE__, ret);
		uart_unregister_driver(&grlib_apbuart_driver);
		return ret;
	}

	return ret;
}

static void __exit grlib_apbuart_exit(void)
{
	int i;

	for (i = 0; i < UART_NR; i++)
		if (test_bit(i, apbuart_ports_in_use))
			uart_remove_one_port(&grlib_apbuart_driver,
					     &grlib_apbuart_ports[i]);

	bitmap_zero(apbuart_ports_in_use, UART_NR);

	uart_unregister_driver(&grlib_apbuart_driver);
	platform_driver_unregister(&grlib_apbuart_of_driver);
}

module_init(grlib_apbuart_init);
module_exit(grlib_apbuart_exit);

MODULE_AUTHOR("Aeroflex Gaisler AB");
MODULE_DESCRIPTION("GRLIB APBUART serial driver");
MODULE_VERSION("2.1");
MODULE_LICENSE("GPL");
