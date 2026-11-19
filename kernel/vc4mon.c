// SPDX-License-Identifier: GPL-2.0-only
/*
 * vc4mon - sysfs window onto the VideoCore IV (V3D) register block.
 *
 * The V3D register block lives at 0x3fc00000 on BCM2835/2836/2837.  The
 * vc4 DRM driver owns the "brcm,vc4-v3d" platform device and has the block
 * ioremapped.  This module deliberately does NOT register a platform driver,
 * does NOT bind/unbind vc4_v3d and does NOT claim the memory resource: it
 * looks the device tree node up, takes the translated MMIO resource, and
 * creates a second ioremap of the same 4 KiB window.
 *
 * The interface is read-only except for two deliberately narrow counter
 * controls:
 *
 *   perf_enable  (0200)  write 0 to disable, or a value inside
 *                        VC4MON_PCTRE_MASK to write PCTRE.
 *   perf_clear   (0200)  write 1 to write PCTRC and wait (bounded) for the
 *                        hardware to self-clear the clear bits.
 *
 * No other register is writable through sysfs.  In particular there is no
 * path to SRQCS, the PCTR/PCTRS index or data registers, VPMBASE, L2CACTL,
 * SLCACTL, reset, clock or power.  PCTRE only enables the 16 counter latches
 * and the documented "enable" bit; PCTRC only clears those latches.  Neither
 * register can generate DMA, write memory, influence rendering, or change a
 * clock or power domain.  A stuck counter can at most keep counting.
 *
 * The module also initialises the counter source map at load time *only* when
 * all 16 PCTRS registers read as zero (the hardware reset value).  That gives
 * an unprivileged sampler the documented primary QPU/GPU mapping (sources
 * 13..28) without exposing a PCTRS write attribute to userspace.  An existing
 * map (for example one programmed by a root vc4top run) is left untouched.
 *
 * Counter ownership: sysfs has no client identity, so a nonzero perf_enable
 * write takes a module-level exclusive lease keyed by current->pid.  A second
 * process gets -EBUSY while the lease is held; the lease expires after
 * VC4MON_LEASE_MS so a crashed sampler cannot lock the counters forever.  The
 * same process may update its own lease.  Writing 0 releases it.
 *
 * Note on memcpy_fromio(): lib/iomap.c implements a 4-byte copy as four
 * __raw_readb() accesses.  The V3D block only returns valid data on 32-bit
 * accesses (byte reads at +1..+3 come back as zero), so a 4-byte
 * memcpy_fromio() yields just the low byte.  Bulk memcpy_fromio() would use
 * 64-bit reads; V3D is a 32-bit register block and readl() is what the DRM
 * driver itself uses.  ioread32() is therefore the correct and safe accessor
 * here.
 *
 * Attributes (/sys/kernel/vc4mon/):
 *   ident        0444  IDENT0/IDENT1/IDENT2 as text
 *   regs         0444  fixed-format dump of the 78 documented V3D registers,
 *                      one "%05x %08x" line per register (offset value)
 *   perf         0444  fixed-format dump of PCTR0..15 and PCTRS0..15
 *   perf_enable  0200  PCTRE enable mask (0 disables; bit 31 is required on
 *                      this hardware for the counters to advance)
 *   perf_clear   0200  write 1 to clear all 16 counters
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sched.h>
#include <linux/sysfs.h>

#define VC4MON_COMPAT	"brcm,vc4-v3d"
#define VC4MON_WINDOW	0x1000	/* one page covers every documented register */
#define VC4MON_MAXREGS	78

#define V3D_PCTRC	0x00670
#define V3D_PCTRE	0x00674
#define V3D_PCTR0	0x00680
#define V3D_PCTRS0	0x00684

/* PCTRE bit 31 is marked reserved by Table 84, but this V3D needs it set. */
#define VC4MON_PCTRE_EN		BIT(31)
#define VC4MON_PCTRE_MASK	(VC4MON_PCTRE_EN | 0xffffu)
#define VC4MON_PCTRC_ALL	0xffffu
#define VC4MON_COUNTERS		16
#define VC4MON_CLEAR_POLL_US	50
#define VC4MON_CLEAR_TIMEOUT_US	10000
#define VC4MON_LEASE_MS		30000

struct vc4mon_reg {
	u16 off;
	const char *name;
};

/* Keep in sync with vc4top's vc4_defines.h registers[] table (78 entries). */
static const struct vc4mon_reg vc4mon_regs[VC4MON_MAXREGS] = {
	{ 0x00000, "V3D_IDENT0" },
	{ 0x00004, "V3D_IDENT1" },
	{ 0x00008, "V3D_IDENT2" },
	{ 0x00010, "V3D_SCRATCH" },
	{ 0x00020, "V3D_L2CACTL" },
	{ 0x00024, "V3D_SLCACTL" },
	{ 0x00030, "V3D_INTCTL" },
	{ 0x00034, "V3D_INTENA" },
	{ 0x00038, "V3D_INTDIS" },
	{ 0x00100, "V3D_CT0CS" },
	{ 0x00104, "V3D_CT1CS" },
	{ 0x00108, "V3D_CT0EA" },
	{ 0x0010c, "V3D_CT1EA" },
	{ 0x00110, "V3D_CT0CA" },
	{ 0x00114, "V3D_CT1CA" },
	{ 0x00118, "V3D_CT00RA0" },
	{ 0x0011c, "V3D_CT01RA0" },
	{ 0x00120, "V3D_CT0LC" },
	{ 0x00124, "V3D_CT1LC" },
	{ 0x00128, "V3D_CT0PC" },
	{ 0x0012c, "V3D_CT1PC" },
	{ 0x00130, "V3D_PCS" },
	{ 0x00134, "V3D_BFC" },
	{ 0x00138, "V3D_RFC" },
	{ 0x00300, "V3D_BPCA" },
	{ 0x00304, "V3D_BPCS" },
	{ 0x00308, "V3D_BPOA" },
	{ 0x0030c, "V3D_BPOS" },
	{ 0x00310, "V3D_BXCF" },
	{ 0x00410, "V3D_SQRSV0" },
	{ 0x00414, "V3D_SQRSV1" },
	{ 0x00418, "V3D_SQCNTL" },
	{ 0x00430, "V3D_SRQPC" },
	{ 0x00434, "V3D_SRQUA" },
	{ 0x00438, "V3D_SRQUL" },
	{ 0x0043c, "V3D_SRQCS" },
	{ 0x00500, "V3D_VPACNTL" },
	{ 0x00504, "V3D_VPMBASE" },
	{ 0x00670, "V3D_PCTRC" },
	{ 0x00674, "V3D_PCTRE" },
	{ 0x00680, "V3D_PCTR0" },
	{ 0x00684, "V3D_PCTRS0" },
	{ 0x00688, "V3D_PCTR1" },
	{ 0x0068c, "V3D_PCTRS1" },
	{ 0x00690, "V3D_PCTR2" },
	{ 0x00694, "V3D_PCTRS2" },
	{ 0x00698, "V3D_PCTR3" },
	{ 0x0069c, "V3D_PCTRS3" },
	{ 0x006a0, "V3D_PCTR4" },
	{ 0x006a4, "V3D_PCTRS4" },
	{ 0x006a8, "V3D_PCTR5" },
	{ 0x006ac, "V3D_PCTRS5" },
	{ 0x006b0, "V3D_PCTR6" },
	{ 0x006b4, "V3D_PCTRS6" },
	{ 0x006b8, "V3D_PCTR7" },
	{ 0x006bc, "V3D_PCTRS7" },
	{ 0x006c0, "V3D_PCTR8" },
	{ 0x006c4, "V3D_PCTRS8" },
	{ 0x006c8, "V3D_PCTR9" },
	{ 0x006cc, "V3D_PCTRS9" },
	{ 0x006d0, "V3D_PCTR10" },
	{ 0x006d4, "V3D_PCTRS10" },
	{ 0x006d8, "V3D_PCTR11" },
	{ 0x006dc, "V3D_PCTRS11" },
	{ 0x006e0, "V3D_PCTR12" },
	{ 0x006e4, "V3D_PCTRS12" },
	{ 0x006e8, "V3D_PCTR13" },
	{ 0x006ec, "V3D_PCTRS13" },
	{ 0x006f0, "V3D_PCTR14" },
	{ 0x006f4, "V3D_PCTRS14" },
	{ 0x006f8, "V3D_PCTR15" },
	{ 0x006fc, "V3D_PCTRS15" },
	{ 0x00f00, "V3D_DBGE" },
	{ 0x00f04, "V3D_FDBGO" },
	{ 0x00f08, "V3D_FDBGB" },
	{ 0x00f0c, "V3D_FDBGR" },
	{ 0x00f10, "V3D_FDBGS" },
	{ 0x00f20, "V3D_ERRSTAT" },
};

static void __iomem *vc4mon_base;
static struct kobject *vc4mon_kobj;
static DEFINE_MUTEX(vc4mon_lock);

/* Protected by vc4mon_lock. */
static bool vc4mon_enabled;
static pid_t vc4mon_owner;
static unsigned long vc4mon_enabled_at;

static u32 vc4mon_read(u32 off)
{
	return ioread32(vc4mon_base + off);
}

static void vc4mon_write(u32 off, u32 val)
{
	iowrite32(val, vc4mon_base + off);
}

static bool vc4mon_lease_expired(void)
{
	return !vc4mon_enabled ||
	       time_after(jiffies,
			  vc4mon_enabled_at + msecs_to_jiffies(VC4MON_LEASE_MS));
}

static ssize_t ident_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	ssize_t n = 0;

	if (mutex_lock_interruptible(&vc4mon_lock))
		return -ERESTARTSYS;
	n += sysfs_emit_at(buf, n, "IDENT0 0x%08x\n", vc4mon_read(0x00000));
	n += sysfs_emit_at(buf, n, "IDENT1 0x%08x\n", vc4mon_read(0x00004));
	n += sysfs_emit_at(buf, n, "IDENT2 0x%08x\n", vc4mon_read(0x00008));
	mutex_unlock(&vc4mon_lock);

	return n;
}

static ssize_t regs_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	ssize_t n = 0;
	size_t i;

	if (mutex_lock_interruptible(&vc4mon_lock))
		return -ERESTARTSYS;
	for (i = 0; i < ARRAY_SIZE(vc4mon_regs); i++) {
		u32 val = vc4mon_read(vc4mon_regs[i].off);

		n += sysfs_emit_at(buf, n, "%05x %08x\n",
				   vc4mon_regs[i].off, val);
	}
	mutex_unlock(&vc4mon_lock);

	return n;
}

static ssize_t perf_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	ssize_t n = 0;
	unsigned int i;

	if (mutex_lock_interruptible(&vc4mon_lock))
		return -ERESTARTSYS;
	for (i = 0; i < VC4MON_COUNTERS; i++)
		n += sysfs_emit_at(buf, n, "PCTR%-2u 0x%08x\n", i,
				   vc4mon_read(V3D_PCTR0 + 8 * i));
	for (i = 0; i < VC4MON_COUNTERS; i++)
		n += sysfs_emit_at(buf, n, "PCTRS%-2u 0x%08x\n", i,
				   vc4mon_read(V3D_PCTRS0 + 8 * i));
	mutex_unlock(&vc4mon_lock);

	return n;
}

/*
 * perf_enable: write PCTRE.  Only bits 0..15 (the counter enables) and bit
 * 31 (required by this V3D) are accepted; every other bit is -EINVAL.  A
 * nonzero write claims the exclusive sampler lease.
 */
static ssize_t perf_enable_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val & ~VC4MON_PCTRE_MASK)
		return -EINVAL;

	if (mutex_lock_interruptible(&vc4mon_lock))
		return -ERESTARTSYS;

	if (!vc4mon_lease_expired() && vc4mon_owner != current->pid) {
		ret = -EBUSY;
		goto out;
	}

	if (!val) {
		vc4mon_write(V3D_PCTRE, 0);
		vc4mon_enabled = false;
		vc4mon_owner = 0;
	} else {
		/*
		 * A stale lease is reclaimed here: the previous sampler is
		 * gone, so its enable has no owner.  Clear that state before
		 * applying the new mask.
		 */
		if (vc4mon_enabled && vc4mon_lease_expired())
			vc4mon_write(V3D_PCTRE, 0);
		vc4mon_write(V3D_PCTRE, val);
		vc4mon_enabled = true;
		vc4mon_owner = current->pid;
		vc4mon_enabled_at = jiffies;
	}
	ret = count;
out:
	mutex_unlock(&vc4mon_lock);
	return ret;
}

/*
 * perf_clear: write 1.  PCTRC is a self-clearing write-one-to-clear register;
 * wait for it to read back zero, but never longer than VC4MON_CLEAR_TIMEOUT_US.
 */
static ssize_t perf_clear_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;
	unsigned int waited = 0;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val != 1)
		return -EINVAL;

	if (mutex_lock_interruptible(&vc4mon_lock))
		return -ERESTARTSYS;

	if (!vc4mon_lease_expired() && vc4mon_owner != current->pid) {
		ret = -EBUSY;
		goto out;
	}

	vc4mon_write(V3D_PCTRC, VC4MON_PCTRC_ALL);
	while (vc4mon_read(V3D_PCTRC) & VC4MON_PCTRC_ALL) {
		if (waited >= VC4MON_CLEAR_TIMEOUT_US) {
			ret = -ETIMEDOUT;
			goto out;
		}
		usleep_range(VC4MON_CLEAR_POLL_US,
			     2 * VC4MON_CLEAR_POLL_US);
		waited += VC4MON_CLEAR_POLL_US;
	}
	ret = count;
out:
	mutex_unlock(&vc4mon_lock);
	return ret;
}

static struct kobj_attribute vc4mon_ident_attr = __ATTR_RO(ident);
static struct kobj_attribute vc4mon_regs_attr = __ATTR_RO(regs);
static struct kobj_attribute vc4mon_perf_attr = __ATTR_RO(perf);
static struct kobj_attribute vc4mon_perf_enable_attr =
	__ATTR(perf_enable, 0200, NULL, perf_enable_store);
static struct kobj_attribute vc4mon_perf_clear_attr =
	__ATTR(perf_clear, 0200, NULL, perf_clear_store);

static struct attribute *vc4mon_attrs[] = {
	&vc4mon_ident_attr.attr,
	&vc4mon_regs_attr.attr,
	&vc4mon_perf_attr.attr,
	&vc4mon_perf_enable_attr.attr,
	&vc4mon_perf_clear_attr.attr,
	NULL,
};

static const struct attribute_group vc4mon_group = {
	.attrs = vc4mon_attrs,
};

/*
 * Give a fresh module load the documented primary mapping (counter i counts
 * source 13+i) only if the hardware reset value (all zero) is still in place.
 * No userspace write path to PCTRS is exposed.
 */
static void vc4mon_init_counter_map(void)
{
	unsigned int i;

	for (i = 0; i < VC4MON_COUNTERS; i++)
		if (vc4mon_read(V3D_PCTRS0 + 8 * i))
			return;
	for (i = 0; i < VC4MON_COUNTERS; i++)
		vc4mon_write(V3D_PCTRS0 + 8 * i, 13 + i);
}

static int __init vc4mon_init(void)
{
	struct device_node *np;
	struct resource res;
	resource_size_t end;
	int ret;

	np = of_find_compatible_node(NULL, NULL, VC4MON_COMPAT);
	if (!np)
		return -ENODEV;

	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;

	if (resource_size(&res) < VC4MON_WINDOW) {
		pr_err("vc4mon: V3D window is only %pap bytes\n",
		       &res.start);
		return -EINVAL;
	}

	/*
	 * Plain ioremap(), not devm_ioremap_resource(): the region is already
	 * owned by vc4_v3d, and request_mem_region() would fail (or, worse,
	 * require the driver to give it up).  A second mapping of the same
	 * device memory does not affect the DRM driver.
	 */
	vc4mon_base = ioremap(res.start, VC4MON_WINDOW);
	if (!vc4mon_base)
		return -ENOMEM;

	vc4mon_init_counter_map();

	vc4mon_kobj = kobject_create_and_add("vc4mon", kernel_kobj);
	if (!vc4mon_kobj) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	ret = sysfs_create_group(vc4mon_kobj, &vc4mon_group);
	if (ret)
		goto err_kobj;

	end = res.start + VC4MON_WINDOW - 1;
	pr_info("vc4mon: V3D regs %pa..%pa mapped; only PCTRE/PCTRC writable\n",
		&res.start, &end);
	return 0;

err_kobj:
	kobject_put(vc4mon_kobj);
	vc4mon_kobj = NULL;
err_unmap:
	iounmap(vc4mon_base);
	vc4mon_base = NULL;
	return ret;
}

static void __exit vc4mon_exit(void)
{
	if (vc4mon_kobj) {
		sysfs_remove_group(vc4mon_kobj, &vc4mon_group);
		kobject_put(vc4mon_kobj);
		vc4mon_kobj = NULL;
	}
	if (vc4mon_base) {
		mutex_lock(&vc4mon_lock);
		/* Unload must never leave the counters enabled, whoever set them. */
		vc4mon_write(V3D_PCTRE, 0);
		vc4mon_enabled = false;
		vc4mon_owner = 0;
		mutex_unlock(&vc4mon_lock);
		pr_info("vc4mon: PCTRE cleared on unload\n");
		iounmap(vc4mon_base);
		vc4mon_base = NULL;
	}
	pr_info("vc4mon: unloaded, V3D registers unmapped\n");
}

module_init(vc4mon_init);
module_exit(vc4mon_exit);

MODULE_AUTHOR("rughinnit");
MODULE_DESCRIPTION("V3D register window with a narrow PCTRE/PCTRC write path");
MODULE_LICENSE("GPL");
