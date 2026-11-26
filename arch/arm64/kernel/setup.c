// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/libfdt.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

static int num_standard_resources;
static struct resource *standard_resources;

phys_addr_t __fdt_pointer __initdata;

/*
 * Standard memory resources
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * The recorded values of x0 .. x3 upon kernel entry.
 */
u64 __cacheline_aligned boot_args[4];

void __init smp_setup_processor_id(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

struct mpidr_hash mpidr_hash;
/**
 * smp_build_mpidr_hash - Pre-compute shifts required at each affinity
 *			  level in order to build a linear index from an
 *			  MPIDR value. Resulting algorithm is a collision
 *			  free hash carried out through shifting and ORing
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

static void *early_fdt_ptr __initdata;

void __init *get_early_fdt_ptr(void)
{
	return early_fdt_ptr;
}

asmlinkage void __init early_fdt_map(u64 dt_phys)
{
	int fdt_size;

	early_fixmap_init();
	early_fdt_ptr = fixmap_remap_fdt(dt_phys, &fdt_size, PAGE_KERNEL);
}

extern char dtb_builtin_begin[];
extern char dtb_builtin_end[];
extern void kmark(u8 c);

extern void kmark_u64(int slot, u64 val);

static u64 __init lk_chosen_u64(const void *fdt, int node, const char *name)
{
	int len;
	const __be32 *p = fdt_getprop(fdt, node, name, &len);

	if (!p)
		return 0;
	if (len == 8)
		return ((u64)be32_to_cpu(p[0]) << 32) | be32_to_cpu(p[1]);
	if (len == 4)
		return be32_to_cpu(p[0]);
	return 0;
}

static char douglas_lk_extra_cmdline[COMMAND_LINE_SIZE] __initdata;

/*
 * The LK bootargs are the vendor's (console=ttyMT0, root=/dev/ram, ...) plus
 * whatever the boot.img cmdline carried. Keep only the postmarketOS tokens
 * (pmos_*, pmos.*) so the initramfs can find its partitions by UUID.
 */
static void __init douglas_pick_lk_cmdline(const void *fdt, int node)
{
	int len;
	const char *args = fdt_getprop(fdt, node, "bootargs", &len);
	const char *p, *end;

	if (!args || len <= 0)
		return;
	end = args + strnlen(args, len);
	for (p = args; p < end;) {
		const char *tok = p;

		while (p < end && *p != ' ')
			p++;
		/*
		 * Forward pmos.* debug switches, but not the pmos_*_uuid pair: a
		 * boot.img from one build must still boot a rootfs from another,
		 * and without the UUIDs the initramfs falls back to the pmOS_boot
		 * and pmOS_root partition labels.
		 */
		if (p - tok > 5 && !strncmp(tok, "pmos", 4) && (tok[4] == '_' || tok[4] == '.') &&
		    !strnstr(tok, "_uuid=", p - tok)) {
			size_t have = strlen(douglas_lk_extra_cmdline);

			if (have + 1 + (p - tok) < sizeof(douglas_lk_extra_cmdline)) {
				douglas_lk_extra_cmdline[have] = ' ';
				memcpy(douglas_lk_extra_cmdline + have + 1, tok, p - tok);
				douglas_lk_extra_cmdline[have + 1 + (p - tok)] = '\0';
			}
		}
		while (p < end && *p == ' ')
			p++;
	}
}

/* Read linux,initrd-start/end (and the pmos_* bootargs) from the FDT LK handed over (x0). */
static void __init douglas_initrd_from_lk_fdt(phys_addr_t lk_phys)
{
	int size = 0, node;
	const void *fdt;
	u64 start, end;

	if (!lk_phys || !IS_ENABLED(CONFIG_BLK_DEV_INITRD))
		return;
	fdt = fixmap_remap_fdt(lk_phys, &size, PAGE_KERNEL_RO);
	if (!fdt)
		return;
	node = fdt_path_offset(fdt, "/chosen");
	if (node < 0)
		return;
	douglas_pick_lk_cmdline(fdt, node);
	start = lk_chosen_u64(fdt, node, "linux,initrd-start");
	end = lk_chosen_u64(fdt, node, "linux,initrd-end");
	if (start && end > start) {
		phys_initrd_start = start;
		phys_initrd_size = end - start;
		kmark_u64(6, start);
		kmark_u64(7, end);
	}
}
extern void kmark_u64(int slot, u64 val);

static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size = 0;
	void *dt_virt;
	const char *name;

	kmark('1');
	kmark_u64(0, dt_phys);
	kmark_u64(1, kimage_voffset);
	/* Use the built-in DTB directly from .rodata (permanently mapped). */
	dt_virt = (void *)dtb_builtin_begin;
	size = (int)(dtb_builtin_end - dtb_builtin_begin);
	kmark('2');
	kmark_u64(4, (u64)dt_virt);
	kmark_u64(5, (u64)size);

	if (dt_virt)
		memblock_reserve(dt_phys, size);
	kmark('3');

	if (!dt_virt || !early_init_dt_scan(dt_virt)) {
		kmark('X');
		pr_crit("\n"
			"Error: invalid device tree blob at physical address %pa (virtual address 0x%px)\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size\n"
			"\nPlease check your bootloader.",
			&dt_phys, dt_virt);

		/*
		 * Note that in this _really_ early stage we cannot even BUG()
		 * or oops, so the least terrible thing to do is cpu_relax(),
		 * or else we could end-up printing non-initialized data, etc.
		 */
		while (true)
			cpu_relax();
	}
	kmark('4');

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	kernel_code.start   = __pa_symbol(_stext);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);
	insert_resource(&iomem_resource, &kernel_code);
	insert_resource(&iomem_resource, &kernel_data);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc(res_size, SMP_CACHE_BYTES);
	if (!standard_resources)
		panic("%s: Failed to allocate %zu bytes\n", __func__, res_size);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		}

		insert_resource(&iomem_resource, res);
	}
}

static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

extern char dtb_builtin_begin[];

/*
 * Uncached DRAM stage markers at 0x98500000 (the 2MB hole, outside all
 * memory blocks - survives every warm reset; readable from TWRP via
 * /dev/physmem). We pin one fixmap slot (the otherwise-unused earlycon
 * slot) to the page for the whole life of the kernel: one PTE write,
 * valid from early_fixmap_init() until forever, no lifecycle headaches
 * with early_ioremap slots. Device-nGnRE attributes mean writes reach
 * DRAM even if the CPU hangs and the LK-armed watchdog hard-resets us.
 */
#define KMARK_PHYS_BASE		0x98500000UL
#define KMARK_MAGIC		0x4B4D524B	/* "KMRK" */

static u8 *kmark_va;
static u8 kmark_idx = 0x10;
static u32 kmark_log_pos;

#define KMARK_LOG_OFF		0x300
#define KMARK_LOG_SIZE		(0x1000 - KMARK_LOG_OFF)

static void __init kmark_init(void)
{
	u32 *p;

	__set_fixmap(FIX_EARLYCON_MEM_BASE, KMARK_PHYS_BASE, pgprot_device(PAGE_KERNEL));
	kmark_va = (u8 *)__fix_to_virt(FIX_EARLYCON_MEM_BASE);

	p = (u32 *)kmark_va;
	if (p[0] == KMARK_MAGIC)
		p[1]++;			/* boots since first flash */
	else {
		p[0] = KMARK_MAGIC;
		p[1] = 1;
	}
	p[2] = 0;			/* reserved */
	dsb(sy);
}

void kmark(u8 c)			/* called from init/main.c too - must not be __init */
{
	if (!kmark_va)
		return;
	writeb(c, kmark_va + kmark_idx++);
	dsb(sy);
}

void kmark_u64(int slot, u64 val)
{
	if (!kmark_va || slot < 0 || slot >= 16)
		return;
	writeq(val, kmark_va + 0x40 + slot * 8);
	dsb(sy);
}

void kmark_log(const char *s, size_t len)
{
	size_t i;

	if (!kmark_va)
		return;
	for (i = 0; i < len; i++) {
		writeb((u8)s[i], kmark_va + KMARK_LOG_OFF + (kmark_log_pos % KMARK_LOG_SIZE));
		kmark_log_pos++;
	}
	writeb('\n', kmark_va + KMARK_LOG_OFF + (kmark_log_pos % KMARK_LOG_SIZE));
	kmark_log_pos++;
	writel(kmark_log_pos, kmark_va + 0x2f8);
	dsb(sy);
}

void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	setup_initial_init_mm(_stext, _etext, _edata, _end);

	*cmdline_p = boot_command_line;

	/*
	 * If know now we are going to need KPTI then use non-global
	 * mappings from the start, avoiding the cost of rewriting
	 * everything later.
	 */
	arm64_use_ng_mappings = kaslr_requires_kpti();

	early_fixmap_init();

	kmark_init();
	kmark('A');			/* fixmap up */

	early_ioremap_init();

	/*
	 * The LK always passes its own 3.18-era vendor DTB, which no mainline
	 * driver can bind. Unconditionally use our built-in douglas DTB
	 * instead (linked into .rodata - see arch/arm64/kernel/dtb_builtin.S).
	 * Before dropping it, take the one thing only LK knows: where it put
	 * the boot.img ramdisk (/chosen/linux,initrd-start|end), so an
	 * external initramfs (postmarketOS) works with the built-in DTB.
	 */
	douglas_initrd_from_lk_fdt(__fdt_pointer);
	__fdt_pointer = __pa_symbol(dtb_builtin_begin);
	kmark_u64(2, (u64)dtb_builtin_begin);
	kmark_u64(3, (u64)(*(u32 *)dtb_builtin_begin));

	setup_machine_fdt(__fdt_pointer);
	kmark('B');			/* DTB scanned, memory/chosen known */
	if (douglas_lk_extra_cmdline[0])
		strlcat(boot_command_line, douglas_lk_extra_cmdline, COMMAND_LINE_SIZE);

	/*
	 * NOTE: do NOT touch the MTK RGU watchdog here. Early experiments
	 * (stage-marker stub stub3) showed that writes to 0x10007000 from
	 * non-secure EL1 abort in this context, killing the kernel silently.
	 * The LK arms the WDT with a ~60s timeout; if we hang, it resets us
	 * and the DRAM trace/log at 0x98400000 survives for post-mortem.
	 */

	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code and early parameters.
	 */
	jump_label_init();
	parse_early_param();
	kmark('C');			/* early params parsed */

	/*
	 * Unmask asynchronous aborts and fiq after bringing up possible
	 * earlycon. (Report possible System Errors once we can report this
	 * occurred).
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	xen_early_init();
	efi_init();

	if (!efi_enabled(EFI_BOOT) && ((u64)_text % MIN_KIMG_ALIGN) != 0)
	     pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");

	arm64_memblock_init();
	kmark('D');			/* memblock ready */

	paging_init();
	kmark('E');			/* MMU/paging up */

	acpi_table_upgrade();

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();

	if (acpi_disabled)
		unflatten_device_tree();

	bootmem_init();

	kasan_init();

	request_standard_resources();

	early_ioremap_reset();

	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	init_bootcpu_ops();
	smp_init_cpus();
	smp_build_mpidr_hash();

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Make sure init_thread_info.ttbr0 always generates translation
	 * faults in case uaccess_enable() is inadvertently called by the init
	 * thread.
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

static int __init topology_init(void)
{
	int i;

	for_each_possible_cpu(i) {
		struct cpu *cpu = &per_cpu(cpu_data.cpu, i);
		cpu->hotpluggable = cpu_can_disable(i);
		register_cpu(cpu, i);
	}

	return 0;
}
subsys_initcall(topology_init);

static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	return 0;
}

static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);
