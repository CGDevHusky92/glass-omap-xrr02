/*
 * clkt_clksel.c - OMAP2/3/4 clksel clock functions
 *
 * Copyright (C) 2005-2008 Texas Instruments, Inc.
 * Copyright (C) 2004-2010 Nokia Corporation
 *
 * Contacts:
 * Richard Woodruff <r-woodruff2@ti.com>
 * Paul Walmsley
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 * clksel clocks are clocks that do not have a fixed parent, or that
 * can divide their parent's rate, or possibly both at the same time, based
 * on the contents of a hardware register bitfield.
 *
 * All of the various mux and divider settings can be encoded into
 * struct clksel* data structures, and then these can be autogenerated
 * from some hardware database for each new chip generation.  This
 * should avoid the need to write, review, and validate a lot of new
 * clock code for each new chip, since it can be exported from the SoC
 * design flow.  This is now done on OMAP4.
 *
 * The fusion of mux and divider clocks is a software creation.  In
 * hardware reality, the multiplexer (parent selection) and the
 * divider exist separately.  XXX At some point these clksel clocks
 * should be split into "divider" clocks and "mux" clocks to better
 * match the hardware.
 *
 * (The name "clksel" comes from the name of the corresponding
 * register field in the OMAP2/3 family of SoCs.)
 *
 * XXX Currently these clocks are only used in the OMAP2/3/4 code, but
 * many of the OMAP1 clocks should be convertible to use this
 * mechanism.
 */
#undef DEBUG

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/clk.h>
#include <linux/io.h>

#include <plat/clock.h>

#include "clock.h"

/* Private functions */

/**
 * _get_clksel_by_parent() - return clksel struct for a given clk & parent
 * @clk: OMAP struct clk ptr to inspect
 * @src_clk: OMAP struct clk ptr of the parent clk to search for
 *
 * Scan the struct clksel array associated with the clock to find
 * the element associated with the supplied parent clock address.
 * Returns a pointer to the struct clksel on success or NULL on error.
 */
static const struct clksel *_get_clksel_by_parent(struct clk *clk,
						  struct clk *src_clk)
{
	const struct clksel *clks;

	for (clks = clk->clksel; clks->parent; clks++)
		if (clks->parent == src_clk)
			break; /* Found the requested parent */

	if (!clks->parent) {
		/* This indicates a data problem */
		WARN(1, "clock: Could not find parent clock %s in clksel array "
		     "of clock %s\n", src_clk->name, clk->name);
		return NULL;
	}

	return clks;
}

/**
 * _get_div_and_fieldval() - find the new clksel divisor and field value to use
 * @src_clk: planned new parent struct clk *
 * @clk: struct clk * that is being reparented
 * @field_val: pointer to a u32 to contain the register data for the divisor
 *
 * Given an intended new parent struct clk * @src_clk, and the struct
 * clk * @clk to the clock that is being reparented, find the
 * appropriate rate divisor for the new clock (returned as the return
 * value), and the corresponding register bitfield data to program to
 * reach that divisor (returned in the u32 pointed to by @field_val).
 * Returns 0 on error, or returns the newly-selected divisor upon
 * success (in this latter case, the corresponding register bitfield
 * value is passed back in the variable pointed to by @field_val)
 */
u8 _get_div_and_fieldval(struct clk *src_clk, struct clk *clk,
				u32 *field_val)
{
	const struct clksel *clks;
	const struct clksel_rate *clkr, *max_clkr = NULL;
	u8 max_div = 0;

	clks = _get_clksel_by_parent(clk, src_clk);
	if (!clks)
		return 0;

	/*
	 * Find the highest divisor (e.g., the one resulting in the
	 * lowest rate) to use as the default.  This should avoid
	 * clock rates that are too high for the device.  XXX A better
	 * solution here would be to try to determine if there is a
	 * divisor matching the original clock rate before the parent
	 * switch, and if it cannot be found, to fall back to the
	 * highest divisor.
	 */
	for (clkr = clks->rates; clkr->div; clkr++) {
		if (!(clkr->flags & cpu_mask))
			continue;

		if (clkr->div > max_div) {
			max_div = clkr->div;
			max_clkr = clkr;
		}
	}

	if (max_div == 0) {
		/* This indicates an error in the clksel data */
		WARN(1, "clock: Could not find divisor for clock %s parent %s"
		     "\n", clk->name, src_clk->parent->name);
		return 0;
	}

	*field_val = max_clkr->val;

	return max_div;
}

/**
 * _write_clksel_reg() - program a clock's clksel register in hardware
 * @clk: struct clk * to program
 * @v: clksel bitfield value to program (with LSB at bit 0)
 *
 * Shift the clksel register bitfield value @v to its appropriate
 * location in the clksel register and write it in.  This function
 * will ensure that the write to the clksel_reg reaches its
 * destination before returning -- important since PRM and CM register
 * accesses can be quite slow compared to ARM cycles -- but does not
 * take into account any time the hardware might take to switch the
 * clock source.
 */
static void _write_clksel_reg(struct clk *clk, u32 field_val)
{
	u32 v;

	v = __raw_readl(clk->clksel_reg);
	v &= ~clk->clksel_mask;
	v |= field_val << __ffs(clk->clksel_mask);
	__raw_writel(v, clk->clksel_reg);

	v = __raw_readl(clk->clksel_reg); /* OCP barrier */
}

/**
 * _clksel_to_divisor() - turn clksel field value into integer divider
 * @clk: OMAP struct clk to use
 * @field_val: register field value to find
 *
 * Given a struct clk of a rate-selectable clksel clock, and a register field
 * value to search for, find the corresponding clock divisor.  The register
 * field value should be pre-masked and shifted down so the LSB is at bit 0
 * before calling.  Returns 0 on error or returns the actual integer divisor
 * upon success.
 */
static u32 _clksel_to_divisor(struct clk *clk, u32 field_val)
{
	const struct clksel *clks;
	const struct clksel_rate *clkr;

	clks = _get_clksel_by_parent(clk, clk->parent);
	if (!clks)
		return 0;

	for (clkr = clks->rates; clkr->div; clkr++) {
		if (!(clkr->flags & cpu_mask))
			continue;

		if (clkr->val == field_val)
			break;
	}

	if (!clkr->div) {
#ifdef CONFIG_OMAP4_DPLL_CASCADING
		pr_debug("clock: Could not find fieldval %d for clock %s parent "
		     "%s\n", field_val, clk->name, clk->parent->name);
#else
		/* This indicates a data error */
		WARN(1, "clock: Could not find fieldval %d for clock %s parent "
		     "%s\n", field_val, clk->name, clk->parent->name);
#endif
		return 0;
	}

	return clkr->div;
}

/**
 * _divisor_to_clksel() - turn clksel integer divisor into a field value
 * @clk: OMAP struct clk to use
 * @div: integer divisor to search for
 *
 * Given a struct clk of a rate-selectable clksel clock, and a clock
 * divisor, find the corresponding register field value.  Returns the
 * register field value _before_ left-shifting (i.e., LSB is at bit
 * 0); or returns 0xFFFFFFFF (~0) upon error.
 */
static u32 _divisor_to_clksel(struct clk *clk, u32 div)
{
	const struct clksel *clks;
	const struct clksel_rate *clkr;

	/* should never happen */
	WARN_ON(div == 0);

	clks = _get_clksel_by_parent(clk, clk->parent);
	if (!clks)
		return ~0;

	for (clkr = clks->rates; clkr->div; clkr++) {
		if (!(clkr->flags & cpu_mask))
			continue;

		if (clkr->div == div)
			break;
	}

	if (!clkr->div) {
		pr_err("clock: Could not find divisor %d for clock %s parent "
		       "%s\n", div, clk->name, clk->parent->name);
		return ~0;
	}

	return clkr->val;
}

/**
 * _read_divisor() - get current divisor applied to parent clock (from hdwr)
 * @clk: OMAP struct clk to use.
 *
 * Read the current divisor register value for @clk that is programmed
 * into the hardware, convert it into the actual divisor value, and
 * return it; or return 0 on error.
 */
static u32 _read_divisor(struct clk *clk)
{
	u32 v;

	if (!clk->clksel || !clk->clksel_mask)
		return 0;

	v = __raw_readl(clk->clksel_reg);
	v &= clk->clksel_mask;
	v >>= __ffs(clk->clksel_mask);

	return _clksel_to_divisor(clk, v);
}

/* Public functions */

/**
 * omap2_clksel_round_rate_div() - find divisor for the given clock and rate
 * @clk: OMAP struct clk to use
 * @target_rate: desired clock rate
 * @new_div: ptr to where we should store the divisor
 *
 * Finds 'best' divider value in an array based on the source and target
 * rates.  The divider array must be sorted with smallest divider first.
 * This function is also used by the DPLL3 M2 divider code.
 *
 * Returns the rounded clock rate or returns 0xffffffff on error.
 */
u32 omap2_clksel_round_rate_div(struct clk *clk, unsigned long target_rate,
				u32 *new_div)
{
	unsigned long test_rate;
	const struct clksel *clks;
	const struct clksel_rate *clkr;
	u32 last_div = 0;
	long last_diff;

	if (!clk->clksel || !clk->clksel_mask)
		return ~0;

	pr_debug("clock: clksel_round_rate_div: %s target_rate %ld\n",
		 clk->name, target_rate);

	*new_div = 1;

	clks = _get_clksel_by_parent(clk, clk->parent);
	if (!clks)
		return ~0;

	last_diff = clk->parent->rate;

	for (clkr = clks->rates; clkr->div; clkr++) {
		long diff;

		if (!(clkr->flags & cpu_mask))
			continue;

		/* Sanity check */
		if (clkr->div <= last_div)
			pr_err("clock: clksel_rate table not sorted "
			       "for clock %s", clk->name);

		last_div = clkr->div;

		test_rate = clk->parent->rate / clkr->div;

		diff = abs(test_rate - target_rate);

		if (test_rate <= target_rate) {
			if (diff > last_diff)
				clkr--;
			break; /* found it */
		}

		last_diff = diff;
	}

	if (!clkr->div) {
		pr_err("clock: Could not find divisor for target "
		       "rate %ld for clock %s parent %s\n", target_rate,
		       clk->name, clk->parent->name);
		return ~0;
	}

	*new_div = clkr->div;

	pr_debug("clock: new_div = %d, new_rate = %ld\n", *new_div,
		 (clk->parent->rate / clkr->div));

	return clk->parent->rate / clkr->div;
}

/*
 * Clocktype interface functions to the OMAP clock code
 * (i.e., those used in struct clk field function pointers, etc.)
 */

/**
 * omap2_init_clksel_parent() - set a clksel clk's parent field from the hdwr
 * @clk: OMAP clock struct ptr to use
 *
 * Given a pointer @clk to a source-selectable struct clk, read the
 * hardware register and determine what its parent is currently set
 * to.  Update @clk's .parent field with the appropriate clk ptr.  No
 * return value.
 */
void omap2_init_clksel_parent(struct clk *clk)
{
	const struct clksel *clks;
	const struct clksel_rate *clkr;
	u32 r, found = 0;

	if (!clk->clksel || !clk->clksel_mask)
		return;

	r = __raw_readl(clk->clksel_reg) & clk->clksel_mask;
	r >>= __ffs(clk->clksel_mask);

	for (clks = clk->clksel; clks->parent && !found; clks++) {
		for (clkr = clks->rates; clkr->div && !found; clkr++) {
			if (!(clkr->flags & cpu_mask))
				continue;

			if (clkr->val == r) {
				if (clk->parent != clks->parent) {
					pr_debug("clock: inited %s parent "
						 "to %s (was %s)\n",
						 clk->name, clks->parent->name,
						 ((clk->parent) ?
						  clk->parent->name : "NULL"));
					clk_reparent(clk, clks->parent);
				};
				found = 1;
			}
		}
	}

	/* This indicates a data error */
	WARN(!found, "clock: %s: init parent: could not find regval %0x\n",
	     clk->name, r);

	return;
}

/**
 * omap2_clksel_recalc() - function ptr to pass via struct clk .recalc field
 * @clk: struct clk *
 *
 * This function is intended to be called only by the clock framework.
 * Each clksel clock should have its struct clk .recalc field set to this
 * function.  Returns the clock's current rate, based on its parent's rate
 * and its current divisor setting in the hardware.
 */
unsigned long omap2_clksel_recalc(struct clk *clk)
{
	unsigned long rate;
	u32 div = 0;

	div = _read_divisor(clk);
	if (div == 0)
		return clk->rate;

	rate = clk->parent->rate / div;

	pr_debug("clock: %s: recalc'd rate is %ld (div %d)\n", clk->name,
		 rate, div);

	return rate;
}

/**
 * omap2_clksel_speculate() - recalc rate from speculative parent clock rate
 * @clk: struct clk * for the clock whose rate we are recalculating
 * @parent_rate: speculative parent clock rate
 *
 * This function takes a speculative or ficticious parent clock rate and
 * recalculates the rate for the clock in question based off of it.  Useful
 * for clock rate change pre-notifiers where we don't want to actually change
 * the clksel, or even the 'rate' field of a clk struct.
 */
unsigned long omap2_clksel_speculate(struct clk *clk, unsigned long parent_rate)
{
	unsigned long rate;
	u32 div = 0;

	/* check for fixed divisor */
	if (clk->fixed_div)
		return parent_rate / clk->fixed_div;

	/* does clock follow parent rate? */
	if (!clk->clksel)
		return parent_rate;

	/* clock must use clksel */
	div = _read_divisor(clk);
	if (div == 0)
		return clk->rate;

	rate = parent_rate / div;

	pr_debug("clock: %s: recalc'd rate is %ld (div %d)\n", clk->name,
		 rate, div);

	return rate;
}

/**
 * omap2_clksel_round_rate() - find rounded rate for the given clock and rate
 * @clk: OMAP struct clk to use
 * @target_rate: desired clock rate
 *
 * This function is intended to be called only by the clock framework.
 * Finds best target rate based on the source clock and possible dividers.
 * rates. The divider array must be sorted with smallest divider first.
 *
 * Returns the rounded clock rate or returns 0xffffffff on error.
 */
long omap2_clksel_round_rate(struct clk *clk, unsigned long target_rate)
{
	u32 new_div;

	return omap2_clksel_round_rate_div(clk, target_rate, &new_div);
}

/**
 * omap2_clksel_set_rate() - program clock rate in hardware
 * @clk: struct clk * to program rate
 * @rate: target rate to program
 *
 * This function is intended to be called only by the clock framework.
 * Program @clk's rate to @rate in the hardware.  The clock can be
 * either enabled or disabled when this happens, although if the clock
 * is enabled, some downstream devices may glitch or behave
 * unpredictably when the clock rate is changed - this depends on the
 * hardware. This function does not currently check the usecount of
 * the clock, so if multiple drivers are using the clock, and the rate
 * is changed, they will all be affected without any notification.
 * Returns -EINVAL upon error, or 0 upon success.
 */
int omap2_clksel_set_rate(struct clk *clk, unsigned long rate)
{
	u32 field_val, validrate, new_div = 0;

	if (!clk->clksel || !clk->clksel_mask)
		return -EINVAL;

	validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
	if (validrate != rate)
		return -EINVAL;

	field_val = _divisor_to_clksel(clk, new_div);
	if (field_val == ~0)
		return -EINVAL;

	_write_clksel_reg(clk, field_val);

	clk->rate = clk->parent->rate / new_div;

	pr_debug("clock: %s: set rate to %ld\n", clk->name, clk->rate);

	return 0;
}

/*
 * Clksel parent setting function - not passed in struct clk function
 * pointer - instead, the OMAP clock code currently assumes that any
 * parent-setting clock is a clksel clock, and calls
 * omap2_clksel_set_parent() by default
 */

/**
 * omap2_clksel_set_parent() - change a clock's parent clock
 * @clk: struct clk * of the child clock
 * @new_parent: struct clk * of the new parent clock
 *
 * This function is intended to be called only by the clock framework.
 * Change the parent clock of clock @clk to @new_parent.  This is
 * intended to be used while @clk is disabled.  This function does not
 * currently check the usecount of the clock, so if multiple drivers
 * are using the clock, and the parent is changed, they will all be
 * affected without any notification.  Returns -EINVAL upon error, or
 * 0 upon success.
 */
int omap2_clksel_set_parent(struct clk *clk, struct clk *new_parent)
{
	u32 field_val = 0;
	u32 parent_div;

	if (!clk->clksel || !clk->clksel_mask)
		return -EINVAL;

	parent_div = _get_div_and_fieldval(new_parent, clk, &field_val);
	if (!parent_div)
		return -EINVAL;

	_write_clksel_reg(clk, field_val);

	clk_reparent(clk, new_parent);

	/* CLKSEL clocks follow their parents' rates, divided by a divisor */
	clk->rate = new_parent->rate;

	if (parent_div > 0)
		clk->rate /= parent_div;

	pr_debug("clock: %s: set parent to %s (new rate %ld)\n",
		 clk->name, clk->parent->name, clk->rate);

	return 0;
}
