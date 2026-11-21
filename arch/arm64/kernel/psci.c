// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "psci: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/mm.h>

#include <uapi/linux/psci.h>

#include <asm/cpu_ops.h>
#include <asm/errno.h>
#include <asm/smp_plat.h>

static int __init cpu_psci_cpu_init(unsigned int cpu)
{
	return 0;
}

static int __init cpu_psci_cpu_prepare(unsigned int cpu)
{
	if (!psci_ops.cpu_on) {
		pr_err("no cpu_on method, not booting CPU%d\n", cpu);
		return -ENODEV;
	}

	return 0;
}

static void mt8163_spm_cpu_power_on(unsigned int cpu)
{
	static void __iomem *spm;
	static const u32 pwr_con_offs[4] = { 0x0200, 0x0218, 0x021c, 0x0220 };
	static const u32 l1_pdn_offs[4]  = { 0x025c, 0x0264, 0x026c, 0x0274 };
	static const u32 sta_bits[4]     = { BIT(9), BIT(10), BIT(11), BIT(12) };
	u32 pwr_con, l1_pdn, sta;
	int to;

	if (cpu == 0 || cpu >= 4)
		return;

	if (!spm) {
		spm = ioremap(0x10006000, 0x1000);
		if (!spm) {
			pr_err("mt8163-smp: failed to ioremap SPM\n");
			return;
		}
	}

	pwr_con = pwr_con_offs[cpu];
	l1_pdn = l1_pdn_offs[cpu];
	sta = sta_bits[cpu];

	/* Enable SPM register control */
	writel((0x0b16U << 16) | BIT(0), spm + 0x0000);

	writel(readl(spm + pwr_con) | BIT(2), spm + pwr_con);	/* PWR_ON */
	udelay(1);
	writel(readl(spm + pwr_con) | BIT(3), spm + pwr_con);	/* PWR_ON_2ND */

	for (to = 10000; to > 0; to--) {
		if ((readl(spm + 0x060c) & sta) && (readl(spm + 0x0610) & sta))
			break;
		udelay(1);
	}
	if (!to) {
		pr_err("mt8163-smp: CPU%u PWR_STATUS timeout\n", cpu);
		return;
	}

	writel(readl(spm + pwr_con) & ~BIT(1), spm + pwr_con);	/* ~PWR_ISO */
	writel(readl(spm + l1_pdn) & ~BIT(0), spm + l1_pdn);	/* ~L1_PDN */

	for (to = 10000; to > 0; to--) {
		if ((readl(spm + l1_pdn) & BIT(8)) == 0)	/* L1_PDN_ACK */
			break;
		udelay(1);
	}
	if (!to) {
		pr_err("mt8163-smp: CPU%u L1_PDN_ACK timeout\n", cpu);
		return;
	}

	udelay(1);
	writel(readl(spm + pwr_con) | BIT(6), spm + pwr_con);	/* SRAM_ISOINT_B */
	writel(readl(spm + pwr_con) & ~BIT(5), spm + pwr_con);	/* ~SRAM_CKISO */
	writel(readl(spm + pwr_con) & ~BIT(4), spm + pwr_con);	/* ~PWR_CLK_DIS */
	writel(readl(spm + pwr_con) | BIT(0), spm + pwr_con);	/* PWR_RST_B */

	pr_info("mt8163-smp: powered on CPU%u via SPM\n", cpu);
}

static int cpu_psci_cpu_boot(unsigned int cpu)
{
	phys_addr_t pa_secondary_entry = __pa_symbol(secondary_entry);
	int err = psci_ops.cpu_on(cpu_logical_map(cpu), pa_secondary_entry);
	if (err)
		pr_err("failed to boot CPU%d (%d)\n", cpu, err);

	if (!err && of_machine_is_compatible("amazon,douglas"))
		mt8163_spm_cpu_power_on(cpu);

	return err;
}

#ifdef CONFIG_HOTPLUG_CPU
static bool cpu_psci_cpu_can_disable(unsigned int cpu)
{
	return !psci_tos_resident_on(cpu);
}

static int cpu_psci_cpu_disable(unsigned int cpu)
{
	/* Fail early if we don't have CPU_OFF support */
	if (!psci_ops.cpu_off)
		return -EOPNOTSUPP;

	/* Trusted OS will deny CPU_OFF */
	if (psci_tos_resident_on(cpu))
		return -EPERM;

	return 0;
}

static void cpu_psci_cpu_die(unsigned int cpu)
{
	/*
	 * There are no known implementations of PSCI actually using the
	 * power state field, pass a sensible default for now.
	 */
	u32 state = PSCI_POWER_STATE_TYPE_POWER_DOWN <<
		    PSCI_0_2_POWER_STATE_TYPE_SHIFT;

	psci_ops.cpu_off(state);
}

static int cpu_psci_cpu_kill(unsigned int cpu)
{
	int err;
	unsigned long start, end;

	if (!psci_ops.affinity_info)
		return 0;
	/*
	 * cpu_kill could race with cpu_die and we can
	 * potentially end up declaring this cpu undead
	 * while it is dying. So, try again a few times.
	 */

	start = jiffies;
	end = start + msecs_to_jiffies(100);
	do {
		err = psci_ops.affinity_info(cpu_logical_map(cpu), 0);
		if (err == PSCI_0_2_AFFINITY_LEVEL_OFF) {
			pr_info("CPU%d killed (polled %d ms)\n", cpu,
				jiffies_to_msecs(jiffies - start));
			return 0;
		}

		usleep_range(100, 1000);
	} while (time_before(jiffies, end));

	pr_warn("CPU%d may not have shut down cleanly (AFFINITY_INFO reports %d)\n",
			cpu, err);
	return -ETIMEDOUT;
}
#endif

const struct cpu_operations cpu_psci_ops = {
	.name		= "psci",
	.cpu_init	= cpu_psci_cpu_init,
	.cpu_prepare	= cpu_psci_cpu_prepare,
	.cpu_boot	= cpu_psci_cpu_boot,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = cpu_psci_cpu_can_disable,
	.cpu_disable	= cpu_psci_cpu_disable,
	.cpu_die	= cpu_psci_cpu_die,
	.cpu_kill	= cpu_psci_cpu_kill,
#endif
};

