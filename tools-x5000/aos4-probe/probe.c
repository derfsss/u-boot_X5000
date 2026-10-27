// SPDX-License-Identifier: GPL-2.0+
/*
 * aos4-probe: a developer tool for checking the AmigaOS 4 boot handoff on
 * the AmigaOne X5000. It is a stand-in Kicklayout EXEC that reports, on the
 * serial port, exactly what a boot loader handed to the AmigaOS 4 kernel
 * loader: the registers, the CPU state, the module list, the board info and
 * any device tree. It can be run under boota or any other loader (BBoot,
 * for example) to compare their handoffs. See README.md.
 *
 * Put "probe_smp" on the command line, or load a module of that name, to
 * also release core 1 through the spin table the device tree advertises.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* DUART1 port 0 (U-Boot's COM1) at the P5020's default CCSRBAR */
#define UART		((volatile u8 *)0xfe11c500)
#define UART_THR	0
#define UART_LSR	5
#define LSR_THRE	0x20

#define RAM_LIMIT	0x80000000u	/* only the low 2 GiB is mapped */

#define mfspr(n) ({ u32 _v; asm volatile("mfspr %0, %1" : "=r"(_v) : "i"(n)); _v; })
#define mtspr(n, v) asm volatile("mtspr %0, %1" : : "i"(n), "r"(v))

static void putc(char c)
{
	if (c == '\n')
		putc('\r');
	while (!(UART[UART_LSR] & LSR_THRE))
		;
	UART[UART_THR] = c;
}

static void puts(const char *s)
{
	while (*s)
		putc(*s++);
}

static void puthex(u32 v, int digits)
{
	while (digits--)
		putc("0123456789abcdef"[(v >> (digits * 4)) & 15]);
}

static void putx(u32 v)
{
	puts("0x");
	puthex(v, 8);
}

static void putdec(u32 v)
{
	char buf[11];
	int i = 0;

	do
		buf[i++] = '0' + v % 10;
	while (v /= 10);
	while (i)
		putc(buf[--i]);
}

static void field(const char *name, u32 v)
{
	puts(name);
	puts(" = ");
	putx(v);
	putc('\n');
}

static int readable(u32 addr, u32 len)
{
	return addr >= 0x100 && addr < RAM_LIMIT && len <= RAM_LIMIT - addr;
}

static u32 rd32(u32 addr)
{
	return *(volatile u32 *)addr;
}

static void hexdump(u32 addr, u32 len)
{
	u32 i;

	if (!readable(addr, len)) {
		puts("  (not readable)\n");
		return;
	}
	for (i = 0; i < len; i += 16) {
		u32 j;

		puts("  ");
		puthex(addr + i, 8);
		puts(":");
		for (j = 0; j < 16 && i + j < len; j += 4) {
			putc(' ');
			puthex(rd32(addr + i + j), 8);
		}
		puts("  ");
		for (j = 0; j < 16 && i + j < len; j++) {
			u8 c = *(volatile u8 *)(addr + i + j);

			putc(c >= 32 && c < 127 ? c : '.');
		}
		putc('\n');
	}
}

/* Print a NUL-terminated string at addr, up to max characters. */
static void putstr_at(u32 addr, u32 max)
{
	u32 i;

	if (!readable(addr, 1)) {
		puts("(not readable)");
		return;
	}
	putc('"');
	for (i = 0; i < max && readable(addr + i, 1); i++) {
		u8 c = *(volatile u8 *)(addr + i);

		if (!c)
			break;
		putc(c >= 32 && c < 127 ? c : '?');
	}
	putc('"');
	if (i == max)
		puts("...");
}

static void dump_cpu(void)
{
	u32 msr;

	asm volatile("mfmsr %0" : "=r"(msr));
	puts("\n== CPU state at entry\n");
	field("MSR    ", msr);
	field("PIR    ", mfspr(286));
	field("PVR    ", mfspr(287));
	field("SVR    ", mfspr(1023));
	field("HID0   ", mfspr(1008));
	field("L1CSR0 ", mfspr(1010));
	field("L1CSR1 ", mfspr(1011));
	field("L2CSR0 ", mfspr(1017));
	field("EPCR   ", mfspr(307));
	field("IVPR   ", mfspr(63));
	field("TCR    ", mfspr(340));
	field("TSR    ", mfspr(336));
	field("DEC    ", mfspr(22));
	field("DEC    ", mfspr(22));
	field("TBU    ", mfspr(269));
	field("TBL    ", mfspr(268));
}

static void dump_tlb1(void)
{
	u32 n = mfspr(689) & 0xfff;	/* TLB1CFG[NENTRY] */
	u32 i;

	puts("\n== TLB1 (valid entries)\n");
	for (i = 0; i < n; i++) {
		u32 mas1, mas2, mas3, mas7;

		mtspr(624, (1 << 28) | (i << 16));	/* MAS0: TLBSEL=1, ESEL=i */
		asm volatile("isync; tlbre; isync");
		mas1 = mfspr(625);
		if (!(mas1 & 0x80000000))
			continue;
		mas2 = mfspr(626);
		mas3 = mfspr(627);
		mas7 = mfspr(944);
		puts("  [");
		putdec(i);
		puts("] MAS1=");
		puthex(mas1, 8);
		puts(" MAS2=");
		puthex(mas2, 8);
		puts(" MAS3=");
		puthex(mas3, 8);
		puts(" MAS7=");
		puthex(mas7, 8);
		puts("  EA ");
		puthex(mas2 & 0xfffff000, 8);
		puts(" -> PA ");
		puthex(mas7 & 0xf, 1);
		puthex(mas3 & 0xfffff000, 8);
		puts(" size ");
		putdec(1u << (2 * ((mas1 >> 8) & 0xf)));	/* 4^TSIZE KiB */
		puts(" KiB");
		puts(mas1 & 0x1000 ? " TS=1" : " TS=0");
		if (mas1 & 0x40000000)
			puts(" IPROT");
		puts(" TID=");
		putdec((mas1 >> 16) & 0x3fff);
		puts(" WIMGE=");
		puthex(mas2 & 0x1f, 2);
		putc('\n');
	}
}

/* --- device tree --------------------------------------------------- */

#define FDT_MAGIC	0xd00dfeed

static u32 smp_release[8];	/* cpu-release-addr for cpu@1.. (low word) */
static u32 smp_count;

static int printable(u32 addr, u32 len)
{
	u32 i;

	if (len < 2 || !*(volatile u8 *)addr || *(volatile u8 *)(addr + len - 1))
		return 0;
	for (i = 0; i + 1 < len; i++) {
		u8 c = *(volatile u8 *)(addr + i);

		if (c && (c < 32 || c >= 127))
			return 0;
	}
	return 1;
}

static int streq(u32 addr, const char *s)
{
	while (*s)
		if (*(volatile u8 *)addr++ != (u8)*s++)
			return 0;
	return *(volatile u8 *)addr == 0;
}

static void dump_fdt(u32 fdt)
{
	u32 size, off_struct, off_strings, p, end;
	int depth = 0, in_cpu = 0;
	u32 cpu_reg = 0;

	if (!readable(fdt, 40) || rd32(fdt) != FDT_MAGIC) {
		puts("  no FDT header\n");
		return;
	}
	size = rd32(fdt + 4);
	off_struct = rd32(fdt + 8);
	off_strings = rd32(fdt + 12);
	puts("  totalsize ");
	putdec(size);
	puts(", version ");
	putdec(rd32(fdt + 20));
	putc('\n');
	if (!readable(fdt, size) || off_struct >= size || off_strings >= size) {
		puts("  bad FDT offsets\n");
		return;
	}
	p = fdt + off_struct;
	end = fdt + size;
	while (p + 4 <= end) {
		u32 tok = rd32(p);
		int i;

		p += 4;
		if (tok == 1) {			/* FDT_BEGIN_NODE */
			u32 name = p;

			for (i = 0; i < depth; i++)
				puts("  ");
			puts("  ");
			putstr_at(name, 64);
			puts(" {\n");
			in_cpu = *(volatile u8 *)name == 'c' &&
				 *(volatile u8 *)(name + 1) == 'p' &&
				 *(volatile u8 *)(name + 2) == 'u' &&
				 *(volatile u8 *)(name + 3) == '@';
			cpu_reg = 0;
			while (*(volatile u8 *)p)
				p++;
			p = (p + 4) & ~3;
			depth++;
		} else if (tok == 2) {		/* FDT_END_NODE */
			depth--;
			in_cpu = 0;
		} else if (tok == 3) {		/* FDT_PROP */
			u32 len = rd32(p), name = fdt + off_strings + rd32(p + 4);
			u32 val = p + 8;
			u32 j;

			for (i = 0; i < depth; i++)
				puts("  ");
			puts("  ");
			putstr_at(name, 64);
			puts(" =");
			if (printable(val, len)) {
				putc(' ');
				putstr_at(val, 128);
			} else {
				for (j = 0; j + 4 <= len && j < 64; j += 4) {
					putc(' ');
					putx(rd32(val + j));
				}
				if (len & 3 || len > 64) {
					puts(" (");
					putdec(len);
					puts(" bytes)");
				}
			}
			putc('\n');
			if (in_cpu && streq(name, "reg") && len >= 4)
				cpu_reg = rd32(val);
			if (in_cpu && cpu_reg && cpu_reg < 9 &&
			    streq(name, "cpu-release-addr") && len == 8) {
				smp_release[cpu_reg - 1] = rd32(val + 4);
				if (cpu_reg > smp_count)
					smp_count = cpu_reg;
			}
			p = (val + len + 3) & ~3;
		} else if (tok == 4) {		/* FDT_NOP */
		} else if (tok == 9) {		/* FDT_END */
			return;
		} else {
			puts("  bad FDT token ");
			putx(tok);
			putc('\n');
			return;
		}
	}
}

/* --- module list (an Exec MinList of the loader's module nodes) ------ */

static u32 dtb_module;
static u32 exec_node, loader_node, smp_module;

static void dump_modules(u32 list)
{
	u32 node, n = 0, total = 0;

	puts("\n== Module list at ");
	putx(list);
	putc('\n');
	if (!readable(list, 12)) {
		puts("  (not readable)\n");
		return;
	}
	puts("  head ");
	putx(rd32(list));
	puts(" tail ");
	putx(rd32(list + 4));
	puts(" tailpred ");
	putx(rd32(list + 8));
	putc('\n');
	for (node = rd32(list); readable(node, 36) &&
	     rd32(node); node = rd32(node)) {
		u32 addr = rd32(node + 28), size = rd32(node + 32);

		puts("  ");
		putdec(n);
		puts(": node ");
		putx(node);
		puts(" id ");
		putdec(rd32(node + 16) >> 16);
		puts(" flags ");
		puthex(rd32(node + 16) & 0xffff, 4);
		puts(" addr ");
		putx(addr);
		puts(" size ");
		putdec(size);
		puts(" opt ");
		putx(rd32(node + 12));
		puts(" alloc ");
		putx(rd32(node + 20));
		puts(" dealloc ");
		putx(rd32(node + 24));
		puts(" first ");
		if (readable(addr, 4))
			puthex(rd32(addr), 8);
		else
			puts("--------");
		putc(' ');
		putstr_at(rd32(node + 8), 48);
		putc('\n');
		if (streq(rd32(node + 8), "dtb"))
			dtb_module = addr;
		if ((rd32(node + 16) >> 16) == 1 && !exec_node)
			exec_node = node;
		if (streq(rd32(node + 8), "loader") && (rd32(node + 16) >> 16) == 2)
			loader_node = node;
		if (streq(rd32(node + 8), "probe_smp"))
			smp_module = node;
		total += size;
		if (++n > 256) {
			puts("  (list does not end, stopping)\n");
			break;
		}
	}
	puts("  ");
	putdec(n);
	puts(" nodes, ");
	putdec(total);
	puts(" bytes of module data\n");
}

/* --- core 1 ---------------------------------------------------------- */

#define CORE1_MAGIC	0xc0de0001

extern char core1_entry[];
static volatile u32 core1_seen[2] __attribute__((aligned(64)));

void core1_main(void)
{
	core1_seen[1] = mfspr(286);	/* PIR */
	asm volatile("sync");
	core1_seen[0] = CORE1_MAGIC;
	asm volatile("dcbf 0, %0; sync" : : "r"(core1_seen) : "memory");
	for (;;)
		;
}

static void release_core1(void)
{
	u32 entry, i;

	puts("\n== SMP: releasing core 1\n");
	if (!smp_count) {
		puts("  no cpu-release-addr for cpu@1 in the device tree\n");
		return;
	}
	entry = smp_release[0];
	puts("  spin table entry ");
	putx(entry);
	putc('\n');
	if (!readable(entry, 64)) {
		puts("  (not readable)\n");
		return;
	}
	hexdump(entry, 32);
	*(volatile u32 *)(entry + 12) = 1;	/* r3 */
	*(volatile u32 *)(entry + 0) = 0;	/* addr, upper */
	asm volatile("sync");
	*(volatile u32 *)(entry + 4) = (u32)core1_entry;
	asm volatile("dcbf 0, %0; sync" : : "r"(entry) : "memory");
	for (i = 0; i < 50000000; i++) {
		asm volatile("dcbf 0, %0; sync" : : "r"(core1_seen) : "memory");
		if (core1_seen[0] == CORE1_MAGIC)
			break;
	}
	if (core1_seen[0] == CORE1_MAGIC) {
		puts("  core 1 is running, PIR ");
		putdec(core1_seen[1]);
		putc('\n');
	} else {
		puts("  core 1 did not respond\n");
	}
	hexdump(entry, 32);
}

/* --- hand over to the real Kickstart/loader --------------------------- */

void chain_to(u32 entry, u32 r3, u32 r4, u32 r5, u32 r6, u32 sp, u32 lr)
	__attribute__((noreturn));

#define wr32(a, v)	(*(volatile u32 *)(a) = (v))

/*
 * The test Kicklayout loads the real loader as an extra MODULE. Make the
 * list look as if it had been the EXEC (the EXEC node now describes it and
 * its own MODULE node is removed), then enter it with the registers and
 * stack we were given, so the machine boots on as normal.
 */
static void chain_loader(u32 r3, u32 r4, u32 r5, u32 r6, u32 sp, u32 lr)
{
	u32 addr = rd32(loader_node + 28), size = rd32(loader_node + 32);
	u32 succ = rd32(loader_node), pred = rd32(loader_node + 4);
	u32 a;

	wr32(pred, succ);
	wr32(succ + 4, pred);
	if (exec_node) {
		wr32(exec_node + 8, rd32(loader_node + 8));
		wr32(exec_node + 28, addr);
		wr32(exec_node + 32, size);
	}
	for (a = addr & ~63u; a < addr + size; a += 64)
		asm volatile("dcbst 0, %0" : : "r"(a));
	asm volatile("sync");
	for (a = addr & ~63u; a < addr + size; a += 64)
		asm volatile("icbi 0, %0" : : "r"(a));
	asm volatile("sync; isync");

	puts("\n==== aos4-probe: entering Kickstart/loader at ");
	putx(addr);
	puts(" ====\n");
	chain_to(addr, r3, r4, r5, r6, sp, lr);
}

/* --- main ------------------------------------------------------------ */

static int has_word(u32 s, const char *w)
{
	u32 i;

	if (!readable(s, 1))
		return 0;
	for (i = 0; i < 4096 && *(volatile u8 *)(s + i); i++) {
		u32 j = 0;

		while (w[j] && *(volatile u8 *)(s + i + j) == (u8)w[j])
			j++;
		if (!w[j])
			return 1;
	}
	return 0;
}

void probe_main(u32 r3, u32 r4, u32 r5, u32 r6, u32 r1, u32 lr, u32 loaded)
{
	u32 tail_fdt = 0;

	puts("\n\n==== aos4-probe ====\n");
	puts("loaded at ");
	putx(loaded);
	puts(", running at ");
	putx(0x01000000);
	putc('\n');
	field("r1 (sp)", r1);
	field("lr     ", lr);
	puts("r3 = ");
	putx(r3);
	putc(' ');
	if (readable(r3, 4) && rd32(r3) == FDT_MAGIC)
		puts("(FDT)");
	else
		putstr_at(r3, 32);
	putc('\n');
	field("r4     ", r4);
	field("r5     ", r5);
	puts("r6 = ");
	putx(r6);
	putc(' ');
	putstr_at(r6, 512);
	putc('\n');

	dump_cpu();
	dump_tlb1();
	dump_modules(r4);

	puts("\n== Board info at r5\n");
	hexdump(r5, 96);
	if (readable(r5, 72)) {
		field("  +0  memstart", rd32(r5 + 0));
		field("  +4  memsize ", rd32(r5 + 4));
		field("  +44 intfreq ", rd32(r5 + 44));
		field("  +48 busfreq ", rd32(r5 + 48));
		field("  +52 baudrate", rd32(r5 + 52));
		field("  +60 magic   ", rd32(r5 + 60));
		field("  +64 u16,u16 ", rd32(r5 + 64));
		field("  +68 pointer ", rd32(r5 + 68));
		if (rd32(r5 + 60) == 0x12345678) {
			tail_fdt = rd32(r5 + 68);
			puts("  data at +68:\n");
			hexdump(tail_fdt, 64);
		}
	}

	if (readable(r3, 4) && rd32(r3) == FDT_MAGIC) {
		puts("\n== FDT at r3\n");
		dump_fdt(r3);
	}
	if (dtb_module) {
		puts("\n== FDT in the \"dtb\" module\n");
		dump_fdt(dtb_module);
	}
	if (tail_fdt && tail_fdt != dtb_module && readable(tail_fdt, 4) &&
	    rd32(tail_fdt) == FDT_MAGIC) {
		puts("\n== FDT at board info +68\n");
		dump_fdt(tail_fdt);
	}

	if (has_word(r6, "probe_smp") || smp_module)
		release_core1();
	else if (loader_node)
		chain_loader(r3, r4, r5, r6, r1, lr);

	puts("\n==== aos4-probe done, reset the machine ====\n");
}
