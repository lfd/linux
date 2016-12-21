/*
 * Clock driver for shared clock devices as jailhouse guests
 *
 * This is only a test driver, with a strong focus on Jetson TK1
 *
 * Copyright (c) OTH Regensburg, 2016
 *
 * Authors:
 *   Ralf Ramsauer <ralf.ramsauer@oth-regensburg.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/reset-controller.h>

static void __iomem *clk_base;

struct regs {
	unsigned rst_set;
	unsigned rst_clr;

	unsigned clk_set;
	unsigned clk_clr;

	unsigned num;
};

struct jailhouse_clk_gate {
	struct clk_hw hw;
	int id; /* gate ID */
	void __iomem *base;
	struct regs regs;
};

struct jailhouse_gate {
	const char *name;
	const char *parent_name;
	const struct regs regs;
};

#define to_jailhouse_clk_gate(_hw) \
	container_of(_hw, struct jailhouse_clk_gate, hw)

#define GATE(_name, _parent, _rst_set, _rst_clr, _clk_set, _clk_clr, _num) { \
		.name = _name, \
		.parent_name = _parent, \
		.regs = { \
			.rst_set = _rst_set, \
			.rst_clr = _rst_clr, \
			.clk_set = _clk_set, \
			.clk_clr = _clk_clr, \
			.num = _num, \
		}, \
	}


static const struct jailhouse_gate gates[] = {
	GATE("I2C1", "I2C",   0x300, 0x304, 0x320, 0x324, 12), /* I2C1 of Jetson TK1 */
	GATE("I2C2", "I2C",   0x308, 0x30c, 0x328, 0x32c, 22), /* I2C2 of Jetson TK1 */
	GATE("DMA", "APBDMA", 0x308, 0x30c, 0x328, 0x32c, 2), /* APBDMA of Jetson TK1 */
	GATE("SPI1", "SPI",   0x308, 0x30c, 0x328, 0x32c, 9), /* SPI1 of Jetson TK1 */
};
#define JAILHOUSE_NR_CLOCKS ARRAY_SIZE(gates)

#define periph_clk_to_bit(gate) (1 << (gate->regs.num % 32))

#define read_enb(gate) \
	readl_relaxed(gate->base + gate->regs.clk_set)

#define read_rst(gate) \
	readl_relaxed(gate->base + gate->regs.rst_set)

/* Stubs for the reset controller. Not used yet. */
static int assert(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	const struct jailhouse_gate *gate;

	pr_debug(" JH: assert reset %lu\n", id);

	if (id >= JAILHOUSE_NR_CLOCKS)
		return -ENOENT;

	gate = gates + id;

	writel_relaxed(periph_clk_to_bit(gate),
		       clk_base + gate->regs.rst_set);

	udelay(2);

	return 0;
}

static int deassert(struct reset_controller_dev *rcdev,
		unsigned long id)
{
	const struct jailhouse_gate *gate;

	pr_debug(" JH: deassert reset %lu\n", id);

	if (id >= JAILHOUSE_NR_CLOCKS)
		return -ENOENT;

	gate = gates + id;

	writel_relaxed(periph_clk_to_bit(gate),
		       clk_base + gate->regs.rst_clr);

	return 0;
}

static const struct reset_control_ops rst_ops = {
	.assert = assert,
	.deassert = deassert,
};

static struct reset_controller_dev rst_ctlr = {
	.ops = &rst_ops,
	.owner = THIS_MODULE,
	.of_reset_n_cells = 1,
};
/* End Reset controller stubs */

static int is_enabled(struct clk_hw *hw)
{
	struct jailhouse_clk_gate *gate = to_jailhouse_clk_gate(hw);
	int state = 1;

	if (read_enb(gate) & periph_clk_to_bit(gate))
		state = 0;

	pr_debug("JH: is_enabled, ID = %d, State = %d\n", gate->id, state);
	return state;
}

static int enable(struct clk_hw *hw)
{
	struct jailhouse_clk_gate *gate = to_jailhouse_clk_gate(hw);

	pr_debug("JH: enable, ID = %d\n", gate->id);

	/* enable gate */
	writel_relaxed(periph_clk_to_bit(gate),
		       gate->base + gate->regs.clk_set);
	udelay(5); /* original driver: udelay(2) */

	/* check if we have to clear resets */
	if (read_rst(gate) & periph_clk_to_bit(gate)) {
		udelay(5); /* rst propagation delay */
		writel_relaxed(periph_clk_to_bit(gate),
			       gate->base + gate->regs.rst_clr);
	}

	return 0;
}

static void disable(struct clk_hw *hw)
{
	struct jailhouse_clk_gate *gate = to_jailhouse_clk_gate(hw);

	pr_debug("JH: disable, ID = %d\n", gate->id);

	/* disable gate */
	writel_relaxed(periph_clk_to_bit(gate),
		       gate->base + gate->regs.clk_clr);

	udelay(5);
}

static unsigned long recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	pr_debug("JH: recalc_rate: %lu\n", parent_rate);
	return -EIO;
}

static long round_rate(struct clk_hw *hw, unsigned long rate,
		       unsigned long *parent_rate)
{
	struct jailhouse_clk_gate *gate = to_jailhouse_clk_gate(hw);

	pr_debug("JH: round_rate: ID = %d, rate = %lu\n", gate->id, rate);
	return rate;
}

static int set_rate(struct clk_hw *hw, unsigned long rate,
		     unsigned long parent_rate)
{
	struct jailhouse_clk_gate *gate = to_jailhouse_clk_gate(hw);
	u32 val = 0;

	pr_debug("JH: set_rate: ID = %d, rate = %lu\n", gate->id, rate);

	if (gate->id == 3) {
		switch (rate) {
			case 11000000:
				val = 0x49;
				break;
			case 10000000:
				val = 0x50;
				break;
			case  5000000:
				val = 0xa2;
				break;
			case  1000000:
				val = 0xff;
				break;
		}
		if (val)
			writel_relaxed(val, gate->base + 0x134);
		return 0;
	}

	return -EIO;
}

const static struct clk_ops ops = {
	.is_enabled = is_enabled,
	.enable = enable,
	.disable = disable,

	.recalc_rate = recalc_rate, /* really needed? */
	.round_rate = round_rate, /* really needed? */
	.set_rate = set_rate, /* really needed? */
};

static struct clk * __init jailhouse_clock_register_gate(const struct jailhouse_gate *gate, int id)
{
	struct jailhouse_clk_gate *cg;
	struct clk_init_data init;
	struct clk *clk;

	cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	pr_debug(" JH: registering %s -> %s\n", gate->name, gate->parent_name);

	init.name = gate->name;
	init.flags = 0;
	init.parent_names = gate->parent_name ? &gate->parent_name : NULL;
	init.num_parents = gate->parent_name ? 1 : 0;
	init.ops = &ops;

	cg->id = id;
	cg->hw.init = &init;

	cg->base = clk_base;
	cg->regs = gate->regs;

	clk = clk_register(NULL, &cg->hw);
	if (IS_ERR(clk))
		kfree(cg);

	return clk;
}

int jailhouse_register_gates(struct device_node *node,
			     struct clk_onecell_data *clk_data)
{
	int num = ARRAY_SIZE(gates);
	int i;
	struct clk *clk;

	for (i = 0; i < num; i++) {
		const struct jailhouse_gate *gate = gates + i;
		clk = jailhouse_clock_register_gate(gate, i);
		if (IS_ERR(clk)) {
			pr_alert(" JH: failed to register clock\n");
			continue;
		}
		clk_data->clks[i] = clk;
	}

	return 0;
}

static struct clk_onecell_data *jailhouse_alloc_clock_data(unsigned int clk_num)
{
	int i;
	struct clk_onecell_data *clk_data;

	clk_data = kzalloc(sizeof(*clk_data), GFP_KERNEL);
	if (!clk_data)
		return NULL;

	clk_data->clks = kcalloc(clk_num, sizeof(*clk_data->clks), GFP_KERNEL);
	if (!clk_data->clks)
		goto err_out;

	clk_data->clk_num = clk_num;

	for (i = 0; i < clk_num; i++)
		clk_data->clks[i] = ERR_PTR(-ENOENT);

	return clk_data;

err_out:
	kfree(clk_data);

	return NULL;
}

static void jailhouse_dealloc_clock_data(struct clk_onecell_data *clk)
{
	kfree(clk->clks);
	kfree(clk);
}

static void __init jailhouse_clock_init(struct device_node *node)
{
	int ret;
	struct clk_onecell_data *clk_data;

	pr_debug("JH: Jailhouse Clock Init\n");

	/* initialise clock controller */
	clk_base = of_iomap(node, 0);
	if (!clk_base)
		return;

	clk_data = jailhouse_alloc_clock_data(JAILHOUSE_NR_CLOCKS);
	if (!clk_data)
		goto unmap_out;

	ret = jailhouse_register_gates(node, clk_data);
	if (ret)
		goto dealloc_clk_out;

	ret = of_clk_add_provider(node, of_clk_src_onecell_get, clk_data);
	if (ret)
		goto unmap_out;

	/* initialise and register reset controller */
	rst_ctlr.of_node = node;
	rst_ctlr.nr_resets = 20;
	ret = reset_controller_register(&rst_ctlr);
	if (ret)
		pr_alert("JH: registering reset controller failed\n");

	return;

dealloc_clk_out:
	jailhouse_dealloc_clock_data(clk_data);

unmap_out:
	iounmap(clk_base);
	pr_alert("JH: error out\n");
}

CLK_OF_DECLARE(jailhouse_clock, "jailhouse,jailhouse-car",
	       jailhouse_clock_init);
