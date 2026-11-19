// SPDX-License-Identifier: GPL-2.0-only
/*
 * vc4mon - read-only sysfs window onto the VideoCore IV (V3D) register block.
 *
 * The V3D register block lives at 0x3fc00000 on BCM2835/2836/2837.  The
 * vc4 DRM driver owns the "brcm,vc4-v3d" platform device and has the block
 * ioremapped.  This module deliberately does NOT register a platform driver,
 * does NOT bind/unbind vc4_v3d and does NOT claim the memory resource: it
 * looks the device tree node up, takes the translated MMIO resource, and
 * creates a second read-only ioremap of the same 4 KiB window.
 *
 * There is no write path: every attribute is read-only, all register reads go
 * through ioread32() (a 32-bit MMIO load), and nothing in this module issues a
 * store, reset, unbind or clock/power change.  Reads have the same side-effect
 * profile as the existing drm/vc4 debugfs "v3d_regs" file.
 *
 * Note on memcpy_fromio(): lib/iomap.c implements a 4-byte copy as four
 * __raw_readb() accesses.  The V3D block only returns valid data on 32-bit
 * accesses (byte reads at +1..+3 come back as zero), so a 4-byte
 * memcpy_fromio() yields just the low byte.  Bulk memcpy_fromio() would use
 * 64-bit reads; V3D is a 32-bit register block and readl() is what the DRM
 * driver itself uses.  ioread32() is therefore the correct and safe accessor
 * here.
 *
 * Attributes (0444, /sys/kernel/vc4mon/):
 *   ident  IDENT0/IDENT1/IDENT2 as text
 *   regs   fixed-format dump of the 78 documented V3D registers,
 *          one "%05x %08x" line per register (offset value)
 *   perf   fixed-format dump of PCTR0..15 and PCTRS0..15 (read-only
 *          observation; this module never enables or clears counters)
 */

#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sysfs.h>

#define VC4MON_COMPAT	"brcm,vc4-v3d"
#define VC4MON_WINDOW	0x1000	/* one page covers every documented register */
#define VC4MON_MAXREGS	78

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

/* The only hardware accessor in this module.  No writes exist. */
static u32 vc4mon_read(u32 off)
{
	return ioread32(vc4mon_base + off);
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
	/* Observation only: never touch PCTRE/PCTRC/PCTRS. */
	for (i = 0; i < 16; i++)
		n += sysfs_emit_at(buf, n, "PCTR%-2u 0x%08x\n", i,
				   vc4mon_read(0x00680 + 8 * i));
	for (i = 0; i < 16; i++)
		n += sysfs_emit_at(buf, n, "PCTRS%-2u 0x%08x\n", i,
				   vc4mon_read(0x00684 + 8 * i));
	mutex_unlock(&vc4mon_lock);

	return n;
}

static struct kobj_attribute vc4mon_ident_attr = __ATTR_RO(ident);
static struct kobj_attribute vc4mon_regs_attr = __ATTR_RO(regs);
static struct kobj_attribute vc4mon_perf_attr = __ATTR_RO(perf);

static struct attribute *vc4mon_attrs[] = {
	&vc4mon_ident_attr.attr,
	&vc4mon_regs_attr.attr,
	&vc4mon_perf_attr.attr,
	NULL,
};

static const struct attribute_group vc4mon_group = {
	.attrs = vc4mon_attrs,
};

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
	 * require the driver to give it up).  A second read-only mapping of
	 * the same device memory does not affect the DRM driver.
	 */
	vc4mon_base = ioremap(res.start, VC4MON_WINDOW);
	if (!vc4mon_base)
		return -ENOMEM;

	vc4mon_kobj = kobject_create_and_add("vc4mon", kernel_kobj);
	if (!vc4mon_kobj) {
		ret = -ENOMEM;
		goto err_unmap;
	}

	ret = sysfs_create_group(vc4mon_kobj, &vc4mon_group);
	if (ret)
		goto err_kobj;

	end = res.start + VC4MON_WINDOW - 1;
	pr_info("vc4mon: V3D regs %pa..%pa mapped read-only (vc4_v3d untouched)\n",
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
		iounmap(vc4mon_base);
		vc4mon_base = NULL;
	}
	pr_info("vc4mon: unloaded, V3D registers unmapped\n");
}

module_init(vc4mon_init);
module_exit(vc4mon_exit);

MODULE_AUTHOR("rughinnit");
MODULE_DESCRIPTION("Read-only sysfs view of the VideoCore IV V3D register block");
MODULE_LICENSE("GPL");
