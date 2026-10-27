// SPDX-License-Identifier: GPL-2.0+
/*
 * Load an AmigaOS 4 kickstart configuration and enter the kernel loader,
 * for boota (AmigaOne X5000, A1222; tested on both).
 *
 * The EXEC (Kickstart/loader, a flat binary entered at its first byte) is
 * called as
 *	exec("AmigaOS4", module_list, board_info, command_line)
 * with the module list an Exec MinList of module nodes, the EXEC first.
 * board_info is the U-Boot bd_t layout plus an extended tail pointing at
 * a device tree with the CPUs (spin-table release addresses) and memory.
 * Layout and values: docs/booting-amigaos.md.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <blk.h>
#include <cpu_func.h>
#include <fs.h>
#include <part.h>
#include <time.h>
#include <asm/global_data.h>
#include <asm/processor.h>
#include <u-boot/crc.h>
#include <linux/libfdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#ifdef CONFIG_MP
#include <asm/mp.h>
#endif
#include "aos4.h"

DECLARE_GLOBAL_DATA_PTR;

struct aos4_list {
	u32 head;
	u32 tail;		/* always 0 */
	u32 tailpred;
} __packed;

struct aos4_module {
	u32 succ;
	u32 pred;
	u32 name;
	u32 options;
	u16 id;			/* MOD_EXEC or MOD_MODULE */
	u16 flags;
	u32 alloc;		/* unused by the kernel */
	u32 dealloc;
	u32 address;
	u32 size;
} __packed;

#define MOD_EXEC	1
#define MOD_MODULE	2
#define MOD_FLAGS	3

struct aos4_board_info {
	u32 memstart;		/* +0 */
	u32 memsize;		/* +4 */
	u32 unused1[9];
	u32 intfreq;		/* +44 CPU clock, Hz */
	u32 busfreq;		/* +48 bus clock, Hz */
	u32 baudrate;		/* +52 */
	u32 unused2;
	u32 magic;		/* +60 */
	u16 tail_version;	/* +64 */
	u16 size;		/* +66, size of this struct */
	u32 fdt;		/* +68 */
} __packed;

#define BI_MAGIC	0x12345678
#define FDT_SPACE	SZ_8K
#define SPIN_ENTRY	64	/* bytes per spin table entry */

_Static_assert(sizeof(struct aos4_module) == 36, "module node size");
_Static_assert(sizeof(struct aos4_board_info) == 72, "board info size");

struct load_state {
	struct blk_desc *desc;
	struct aos4_entry *e;
	ulong cur;		/* next free byte */
	ulong top;		/* end of the area */
	struct aos4_list *list;
	bool quiet;
};

/*
 * The base of U-Boot's spin table if the secondary cores are parked in it
 * (U-Boot keeps a waiting core's address word at 1), else 0.
 */
static u32 spin_table(void)
{
#ifdef CONFIG_MP
	u32 *table = (u32 *)(ulong)get_spin_phys_addr();

	if (cpu_numcores() > 1 && table[SPIN_ENTRY / 4 + 1] == 1)
		return (u32)(ulong)table;
#endif
	return 0;
}

static void module_add_tail(struct aos4_list *l, struct aos4_module *n)
{
	n->succ = (u32)(ulong)&l->tail;
	n->pred = l->tailpred;
	/* the predecessor's succ: a node's, or the header's head */
	*(u32 *)(ulong)l->tailpred = (u32)(ulong)n;
	l->tailpred = (u32)(ulong)n;
}

/* Kicklayout names are relative to the volume; drop any "VOL:" prefix. */
static void volume_path(const char *file, char *path, size_t len)
{
	const char *colon = strchr(file, ':');

	if (colon)
		file = colon + 1;
	while (*file == '/')
		file++;
	snprintf(path, len, "/%s", file);
}

/* Load one file into the area and append its node. */
static int load_file(struct load_state *s, const char *file, u16 id,
		     struct aos4_module **out)
{
	const char *base = strrchr(file, '/');
	struct aos4_module *node;
	char path[256], *name;
	loff_t size, got;
	ulong data;

	base = base ? base + 1 : file;
	volume_path(file, path, sizeof(path));

	if (fs_set_blk_dev_with_part(s->desc, s->e->part) ||
	    fs_size(path, &size))
		return -ENOENT;

	data = ALIGN(s->cur, 32);
	if (size > s->top - data ||
	    data + (ulong)size + sizeof(*node) + strlen(base) + 1 > s->top)
		return -ENOMEM;
	if (fs_set_blk_dev_with_part(s->desc, s->e->part) ||
	    fs_read(path, data, 0, size, &got) || got != size)
		return -EIO;

	node = (struct aos4_module *)(data + (ulong)size);
	name = (char *)(node + 1);
	strcpy(name, base);
	memset(node, 0, sizeof(*node));
	node->name = (u32)(ulong)name;
	node->id = id;
	node->flags = MOD_FLAGS;
	node->address = (u32)data;
	node->size = (u32)size;
	module_add_tail(s->list, node);
	s->cur = (ulong)name + strlen(name) + 1;
	if (out)
		*out = node;
	return 0;
}

#define BAR_WIDTH	30

/*
 * One line, redrawn in place: the file being loaded, a bar and a
 * percentage. file NULL draws the finished bar.
 */
static void progress(const char *file, int done, int total, bool quiet)
{
	const char *name = file ? strrchr(file, '/') : NULL;
	char bar[BAR_WIDTH + 1];
	int n = done * BAR_WIDTH / total;

	if (quiet)
		return;
	name = name ? name + 1 : file ? file : "";
	memset(bar, '#', n);
	memset(bar + n, '-', BAR_WIDTH - n);
	bar[BAR_WIDTH] = '\0';
	printf("\r  %-28.28s [%s] %3d %%", name, bar, done * 100 / total);
}

/* Ask whether to boot without a module that could not be loaded. */
static bool skip_module(const char *file, int err, bool quiet)
{
	aos4_console_screen();
	printf("\n%s: %s\n", file, err == -ENOENT ? "not found" :
	       err == -ENOMEM ? "does not fit in memory" : "read error");
	if (quiet || err == -ENOMEM) {
		if (err != -ENOMEM)
			printf("Booting without it.\n");
		return err != -ENOMEM;
	}
	printf("Press C to boot without it, A to abort: ");
	for (;;) {
		int c = getchar();

		if (c == 'c' || c == 'C') {
			printf("continue\n");
			return true;
		}
		if (c == 'a' || c == 'A' || c == 0x03) {
			printf("abort\n");
			return false;
		}
	}
}

/* Written front to back (sequential-write API), so nodes keep this order. */
static int build_fdt(void *fdt, u32 memstart, u32 memsize, u32 spin)
{
	u32 reg[4] = { 0, cpu_to_fdt32(memstart), 0, cpu_to_fdt32(memsize) };
	int i, err;

	err = fdt_create(fdt, FDT_SPACE);
	err |= fdt_finish_reservemap(fdt);
	err |= fdt_begin_node(fdt, "");

	err |= fdt_begin_node(fdt, "cpus");
	err |= fdt_property_u32(fdt, "#address-cells", 1);
	err |= fdt_property_u32(fdt, "#size-cells", 0);
	for (i = 0; i < cpu_numcores(); i++) {
		char name[16];

		snprintf(name, sizeof(name), "cpu@%d", i);
		err |= fdt_begin_node(fdt, name);
		err |= fdt_property_string(fdt, "device_type", "cpu");
		err |= fdt_property_u32(fdt, "reg", i);
		err |= fdt_property_string(fdt, "enable-method", "spin-table");
		if (spin)
			err |= fdt_property_u64(fdt, "cpu-release-addr",
						spin + SPIN_ENTRY * i);
		err |= fdt_end_node(fdt);
	}
	err |= fdt_end_node(fdt);

	err |= fdt_begin_node(fdt, "memory");
	err |= fdt_property_string(fdt, "device_type", "memory");
	err |= fdt_property(fdt, "reg", reg, sizeof(reg));
	err |= fdt_end_node(fdt);

	err |= fdt_end_node(fdt);
	return err ? -FDT_ERR_NOSPACE : fdt_finish(fdt);
}

static void print_layout(struct load_state *s, ulong base,
			 struct aos4_board_info *bi, ulong exec, u32 spin,
			 bool files)
{
	struct aos4_module *n;
	int count = 0;

	for (n = (void *)(ulong)s->list->head; n->succ;
	     n = (void *)(ulong)n->succ) {
		if (files)
			printf("  %08x %8u crc32 %08x %s\n", n->address, n->size,
			       crc32(0, (void *)(ulong)n->address, n->size),
			       (char *)(ulong)n->name);
		count++;
	}
	printf("module list %p: %d files, area 0x%08lx..0x%08lx (top 0x%08lx)\n",
	       s->list, count, base, s->cur, s->top);
	printf("entry 0x%08lx, board info %p: memory 0x%08x+0x%08x, cpu %u Hz, bus %u Hz, %u baud\n",
	       exec, bi, bi->memstart, bi->memsize, bi->intfreq, bi->busfreq,
	       bi->baudrate);
	printf("device tree 0x%08x, %u bytes, %d cpus, spin table 0x%08x\n",
	       bi->fdt, fdt_totalsize((void *)(ulong)bi->fdt), cpu_numcores(),
	       spin);
}

int aos4_boot(struct aos4_entry *e, const char *cmdline, bool quiet,
	      bool dry_run)
{
	struct aos4_config *cfg = e->cfg;
	struct aos4_board_info *bi;
	struct aos4_module *exec;
	struct load_state s;
	ulong base, start;
	char *str, *args;
	u32 memstart, memsize, spin;
	int i, err;

	memset(&s, 0, sizeof(s));
	s.e = e;
	s.quiet = quiet;
	s.desc = blk_get_dev(e->ifname, e->devnum);
	if (!s.desc)
		return -ENODEV;

	/*
	 * The kickstart goes in the 64 MiB below U-Boot's stack (where
	 * AmigaBoot also put it). It must stay clear of 0x01000000, where
	 * the kernel loader copies itself.
	 */
	s.top = ((ulong)gd - SZ_1M) & ~(ulong)(SZ_1M - 1);
	base = s.top - SZ_64M;
	s.list = (struct aos4_list *)base;
	s.list->head = (u32)(ulong)&s.list->tail;
	s.list->tail = 0;
	s.list->tailpred = (u32)(ulong)&s.list->head;
	s.cur = base + sizeof(*s.list);

	if (!quiet)
		printf("Booting %s from %s (%s %d:%d)\n", cfg->label,
		       e->volume, e->ifname, e->devnum, e->part);
	start = get_timer(0);

	progress(cfg->exec, 0, cfg->nmods + 1, quiet);
	err = load_file(&s, cfg->exec, MOD_EXEC, &exec);
	if (err) {
		skip_module(cfg->exec, err, true);
		printf("Cannot boot without the kernel loader.\n");
		return err;
	}
	for (i = 0; i < cfg->nmods; i++) {
		progress(cfg->mods[i], i + 1, cfg->nmods + 1, quiet);
		err = load_file(&s, cfg->mods[i], MOD_MODULE, NULL);
		if (err && !skip_module(cfg->mods[i], err, quiet))
			return err;
	}
	if (!quiet) {
		ulong ms = get_timer(start);

		progress(NULL, 1, 1, quiet);
		printf("\n%d files, %lu KiB, loaded in %lu.%02lu s\n",
		       cfg->nmods + 1, (s.cur - base) / 1024, ms / 1000,
		       ms % 1000 / 10);
	}

	/* RAM for the kernel, ending below U-Boot if a core is parked */
	memstart = (u32)gd->bd->bi_dram[0].start;
	memsize = (u32)min_t(u64, gd->bd->bi_dram[0].size, SZ_2G);
	spin = spin_table();
	if (spin && memstart + memsize > (gd->relocaddr & ~(SZ_1M - 1)))
		memsize = (gd->relocaddr & ~(SZ_1M - 1)) - memstart;

	bi = (struct aos4_board_info *)ALIGN(s.cur, 4);
	str = (char *)ALIGN((ulong)(bi + 1), 8) + FDT_SPACE;
	args = str + sizeof("AmigaOS4");
	if ((ulong)args + strlen(cmdline) + 1 > s.top) {
		aos4_console_screen();
		printf("The kickstart does not fit in memory.\n");
		return -ENOMEM;
	}
	memset(bi, 0, sizeof(*bi));
	bi->memstart = memstart;
	bi->memsize = memsize;
	bi->intfreq = (u32)gd->cpu_clk;
	bi->busfreq = (u32)gd->bus_clk;
	bi->baudrate = gd->baudrate;
	bi->magic = BI_MAGIC;
	bi->tail_version = 0;
	bi->size = sizeof(*bi);
	bi->fdt = (u32)ALIGN((ulong)(bi + 1), 8);
	err = build_fdt((void *)(ulong)bi->fdt, memstart, memsize, spin);
	if (err) {
		aos4_console_screen();
		printf("Device tree: %s\n", fdt_strerror(err));
		return -EINVAL;
	}
	strcpy(str, "AmigaOS4");
	strcpy(args, cmdline);
	s.cur = (ulong)args + strlen(args) + 1;

	if (dry_run) {
		print_layout(&s, base, bi, exec->address, spin, true);
		return 0;
	}

	/* loading is done: the screen clears, the details go to serial */
	aos4_console_restore();
	if (!quiet)
		print_layout(&s, base, bi, exec->address, spin, false);

	flush_cache(base, s.cur - base);

	/*
	 * The kernel loader is entered with U-Boot's MMU setup, exception
	 * vectors, caches and stack still in place. U-Boot's timer tick must
	 * not fire into it, so the decrementer interrupt goes off first
	 * (get_timer() is timebase-based on these boards and keeps working).
	 */
	mtspr(SPRN_TCR, mfspr(SPRN_TCR) & ~TCR_PIE);
	((void (*)(const char *, void *, void *, const char *))
		(ulong)exec->address)(str, s.list, bi, args);

	/* the kernel does not return */
	aos4_console_screen();
	printf("The kernel loader returned.\n");
	return -EFAULT;
}
