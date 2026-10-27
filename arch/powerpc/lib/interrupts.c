// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2000-2002
 * Wolfgang Denk, DENX Software Engineering, wd@denx.de.
 *
 * (C) Copyright 2003
 * Gleb Natapov <gnatapov@mrv.com>
 */

#include <common.h>
#include <irq_func.h>
/* AmigaOne: for the timebase-based get_timer() below */
#include <time.h>
#include <div64.h>
#include <asm/processor.h>
#include <watchdog.h>
#ifdef CONFIG_LED_STATUS
#include <status_led.h>
#endif
#include <asm/ptrace.h>

#ifndef CONFIG_MPC83XX_TIMER
#ifndef CONFIG_SYS_WATCHDOG_FREQ
#define CONFIG_SYS_WATCHDOG_FREQ (CONFIG_SYS_HZ / 2)
#endif

static unsigned decrementer_count; /* count value for 1e6/HZ microseconds */

static __inline__ unsigned long get_dec (void)
{
	unsigned long val;

	asm volatile ("mfdec %0":"=r" (val):);

	return val;
}


static __inline__ void set_dec (unsigned long val)
{
	if (val)
		asm volatile ("mtdec %0"::"r" (val));
}
#endif /* !CONFIG_MPC83XX_TIMER */

void enable_interrupts(void)
{
	set_msr (get_msr () | MSR_EE);
}

/* returns flag if MSR_EE was set before */
int disable_interrupts(void)
{
	ulong msr = get_msr ();

	set_msr (msr & ~MSR_EE);
	return ((msr & MSR_EE) != 0);
}

#ifndef CONFIG_MPC83XX_TIMER
int interrupt_init(void)
{
	/* call cpu specific function from $(CPU)/interrupts.c */
	interrupt_init_cpu (&decrementer_count);

	set_dec (decrementer_count);

	set_msr (get_msr () | MSR_EE);

	return (0);
}

static volatile ulong timestamp = 0;

void timer_interrupt(struct pt_regs *regs)
{
	/* call cpu specific function from $(CPU)/interrupts.c */
	timer_interrupt_cpu (regs);

	/* Restore Decrementer Count */
	set_dec (decrementer_count);

	timestamp++;

#if defined(CONFIG_WATCHDOG) || defined (CONFIG_HW_WATCHDOG)
	if ((timestamp % (CONFIG_SYS_WATCHDOG_FREQ)) == 0)
		WATCHDOG_RESET ();
#endif    /* CONFIG_WATCHDOG || CONFIG_HW_WATCHDOG */

#ifdef CONFIG_LED_STATUS
	status_led_tick(timestamp);
#endif /* CONFIG_LED_STATUS */
}

ulong get_timer (ulong base)
{
#if defined(CONFIG_TARGET_CYRUS_P5020) || defined(CONFIG_TARGET_CYRUS_P5040)
	/*
	 * AmigaOne X5000: milliseconds derived from the timebase instead of
	 * a count of decrementer interrupts. The timebase runs whatever the
	 * interrupt state, so get_timer() keeps working with MSR[EE] clear,
	 * and boota (common/aos4/load.c) can switch the decrementer
	 * interrupt off before entering the AmigaOS kernel loader without
	 * breaking U-Boot's timekeeping.
	 */
	static unsigned long ticks_per_ms;
	unsigned long long t = get_ticks();

	if (!ticks_per_ms)
		ticks_per_ms = (unsigned long)(get_tbclk() / 1000);
	do_div(t, ticks_per_ms);
	return (ulong)t - base;
#else
	return (timestamp - base);
#endif
}
#endif /* !CONFIG_MPC83XX_TIMER */
