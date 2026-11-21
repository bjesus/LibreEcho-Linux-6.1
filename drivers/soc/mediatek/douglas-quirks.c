// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental: replicate SoC-level state the Amazon vendor kernel sets at boot
 * (mt_dcm_init defaults, AP_PLL_CON5) to find what blocks AFE uplink memif data.
 * Variant selected at build time; see HANDOFF.md section 8A.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/printk.h>

#ifndef DOUGLAS_QUIRK_VARIANT
#define DOUGLAS_QUIRK_VARIANT 0
#endif

static void __init q_rmw(void __iomem *b, u32 off, u32 mask, u32 val, const char *name)
{
	u32 old = readl(b + off), new = (old & ~mask) | (val & mask);

	writel(new, b + off);
	pr_info("douglas-quirks: %s %08x -> %08x (read back %08x)\n", name, old, new, readl(b + off));
}

static int __init douglas_quirks_init(void)
{
	void __iomem *infra = ioremap(0x10001000, 0x1000);
	void __iomem *dramc = ioremap(0x10004000, 0x1000);
	void __iomem *apmixed = ioremap(0x1000c000, 0x1000);

	if (!infra || !dramc || !apmixed)
		return -ENOMEM;
	pr_info("douglas-quirks: variant %d\n", DOUGLAS_QUIRK_VARIANT);
#if DOUGLAS_QUIRK_VARIANT == 1
	/* vendor mt_dcm_init(): armcore DCM mode1, misc DCM default (pmic dcm off), EMI DCM */
	q_rmw(infra, 0x10, 0x7 << 4, 0x7 << 4, "INFRA_TOPCKGEN_DCMCTL");
	q_rmw(infra, 0x74, (1 << 29) | (1 << 28) | (1 << 22) | (1 << 21), 0xb0000000, "PERI_BUS_DCM_CTRL misc");
	q_rmw(dramc, 0x1dc, 1 << 25, 1 << 25, "DRAMC_PD_CTRL");
	q_rmw(dramc, 0x828, 1 << 25, 1 << 25, "DFS_DRAMC_PD_CTRL");
#elif DOUGLAS_QUIRK_VARIANT == 2
	{
		/* one-shot guard: WDT_NONRST_REG2 survives a watchdog reset */
		void __iomem *wdt = ioremap(0x10007000, 0x100);
		u32 nr = readl(wdt + 0x24);

		if (nr & BIT(31)) {
			pr_info("douglas-quirks: previous attempt did not survive, SKIPPING AP_PLL_CON5 (nonrst2=%08x)\n", nr);
			writel(nr & ~BIT(31), wdt + 0x24);
		} else {
			writel(nr | BIT(31), wdt + 0x24);
			q_rmw(apmixed, 0x14, 0xffffffff, 0, "AP_PLL_CON5");
		}
		iounmap(wdt);
	}
#endif
	iounmap(infra); iounmap(dramc); iounmap(apmixed);
	return 0;
}
arch_initcall(douglas_quirks_init);

static int __init douglas_quirks_survived(void)
{
	void __iomem *wdt = ioremap(0x10007000, 0x100);

	if (wdt) {
		u32 nr = readl(wdt + 0x24);

		pr_info("douglas-quirks: survived to late_initcall, clearing guard (nonrst2=%08x)\n", nr);
		writel(nr & ~BIT(31), wdt + 0x24);
		iounmap(wdt);
	}
	return 0;
}
late_initcall(douglas_quirks_survived);
