// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU driver for BCM2712
 *
 * Copyright (c) 2023 Raspberry Pi Ltd.
 */

#include "bcm2712-iommu.h"

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MMU_WR(off, val)   writel(val, mmu->reg_base + (off))
#define MMU_RD(off)        readl(mmu->reg_base + (off))

#define domain_to_mmu(d) (container_of(d, struct bcm2712_iommu_domain, base)->mmu)

#define MMMU_CTRL_OFFSET                       0x00
#define MMMU_CTRL_CAP_EXCEEDED                 BIT(27)
#define MMMU_CTRL_CAP_EXCEEDED_ABORT_EN        BIT(26)
#define MMMU_CTRL_CAP_EXCEEDED_INT_EN          BIT(25)
#define MMMU_CTRL_CAP_EXCEEDED_EXCEPTION_EN    BIT(24)
#define MMMU_CTRL_PT_INVALID                   BIT(20)
#define MMMU_CTRL_PT_INVALID_ABORT_EN          BIT(19)
#define MMMU_CTRL_PT_INVALID_EXCEPTION_EN      BIT(18)
#define MMMU_CTRL_PT_INVALID_EN                BIT(17)
#define MMMU_CTRL_WRITE_VIOLATION              BIT(12)
#define MMMU_CTRL_WRITE_VIOLATION_ABORT_EN     BIT(11)
#define MMMU_CTRL_WRITE_VIOLATION_INT_EN       BIT(10)
#define MMMU_CTRL_WRITE_VIOLATION_EXCEPTION_EN BIT(9)
#define MMMU_CTRL_BYPASS                       BIT(8)
#define MMMU_CTRL_TLB_CLEARING                 BIT(7)
#define MMMU_CTRL_STATS_CLEAR                  BIT(3)
#define MMMU_CTRL_TLB_CLEAR                    BIT(2)
#define MMMU_CTRL_STATS_ENABLE                 BIT(1)
#define MMMU_CTRL_ENABLE                       BIT(0)

#define MMMU_PT_PA_BASE_OFFSET                 0x04

#define MMMU_HIT_OFFSET                        0x08
#define MMMU_MISS_OFFSET                       0x0C
#define MMMU_STALL_OFFSET                      0x10

#define MMMU_ADDR_CAP_OFFSET                   0x14
#define MMMU_ADDR_CAP_ENABLE                   BIT(31)
#define ADDR_CAP_SHIFT 28 /* ADDR_CAP is defined to be in 256 MByte units */

#define MMMU_SHOOT_DOWN_OFFSET                 0x18
#define MMMU_SHOOT_DOWN_SHOOTING               BIT(31)
#define MMMU_SHOOT_DOWN_SHOOT                  BIT(30)

#define MMMU_BYPASS_START_OFFSET               0x1C
#define MMMU_BYPASS_START_ENABLE               BIT(31)
#define MMMU_BYPASS_START_INVERT               BIT(30)

#define MMMU_BYPASS_END_OFFSET                 0x20
#define MMMU_BYPASS_END_ENABLE                 BIT(31)

#define MMMU_MISC_OFFSET                       0x24
#define MMMU_MISC_SINGLE_TABLE                 BIT(31)

#define MMMU_ILLEGAL_ADR_OFFSET                0x30
#define MMMU_ILLEGAL_ADR_ENABLE                BIT(31)

#define MMMU_DEBUG_INFO_OFFSET                 0x38
#define MMMU_DEBUG_INFO_VERSION_MASK           0x0000000Fu
#define MMMU_DEBUG_INFO_VA_WIDTH_MASK          0x000000F0u
#define MMMU_DEBUG_INFO_PA_WIDTH_MASK          0x00000F00u
#define MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK     0x000FF000u
#define MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK   0x0FF00000u
#define MMMU_DEBUG_INFO_BYPASS_4M              BIT(28)
#define MMMU_DEBUG_INFO_BYPASS                 BIT(29)

#define MMMU_PTE_PAGESIZE_MASK                 0xC0000000u
#define MMMU_PTE_WRITEABLE                     BIT(29)
#define MMMU_PTE_VALID                         BIT(28)

/*
 * BCM2712 IOMMU is organized around 4Kbyte pages (MMU_PAGE_SIZE).
 * Linux PAGE_SIZE must not be smaller but may be larger (e.g. 4K, 16K).
 *
 * Unlike many larger MMUs, this one uses a 4-byte word size, allowing
 * 1024 entries within each 4K table page, and two-level translation.
 *
 * Let's allocate enough table space for 2GB of translated memory (IOVA).
 * This requires 512 4K pages (2MB) of level-2 tables, one page of
 * top-level table (only half-filled in this particular configuration),
 * plus one "default" page to catch illegal requests.
 *
 * The translated virtual address region is between 40GB and 42GB;
 * addresses below this range pass straight through to the SDRAM.
 *
 * Currently we assume a 1:1:1 correspondence of IOMMU, group and domain.
 */

#define MMU_PAGE_SHIFT    12
#define MMU_PAGE_SIZE     BIT(MMU_PAGE_SHIFT)

#define PAGEWORDS_SHIFT   (MMU_PAGE_SHIFT - 2)
#define HUGEPAGE_SHIFT    (MMU_PAGE_SHIFT + PAGEWORDS_SHIFT)
#define L1_CHUNK_SHIFT    (MMU_PAGE_SHIFT + 2 * PAGEWORDS_SHIFT)

#define APERTURE_BASE     (40ul << 30)
#define APERTURE_SIZE     (2ul << 30)
#define APERTURE_TOP      (APERTURE_BASE + APERTURE_SIZE)
#define TRANSLATED_PAGES  (APERTURE_SIZE >> MMU_PAGE_SHIFT)
#define L2_PAGES          (TRANSLATED_PAGES >> PAGEWORDS_SHIFT)
#define TABLES_ALLOC_SIZE (L2_PAGES * MMU_PAGE_SIZE + 2 * PAGE_SIZE)

static void bcm2712_iommu_init(struct bcm2712_iommu *mmu)
{
	unsigned int i, bypass_shift;
	struct sg_dma_page_iter it;
	u32 u = MMU_RD(MMMU_DEBUG_INFO_OFFSET);

	/*
	 * Check IOMMU version and hardware configuration.
	 * This driver is for VC IOMMU version >= 4 (with 2-level tables)
	 * and assumes at least 36 bits of virtual and physical address space.
	 * Bigpage and superpage sizes are typically 64K and 1M, but may vary
	 * (hugepage size is fixed at 4M, the range covered by an L2 page).
	 */
	dev_info(mmu->dev, "%s: DEBUG_INFO = 0x%08x\n", __func__, u);
	WARN_ON(FIELD_GET(MMMU_DEBUG_INFO_VERSION_MASK, u) < 4 ||
		FIELD_GET(MMMU_DEBUG_INFO_VA_WIDTH_MASK, u) < 6 ||
		FIELD_GET(MMMU_DEBUG_INFO_PA_WIDTH_MASK, u) < 6 ||
		!(u & MMMU_DEBUG_INFO_BYPASS));

	mmu->bigpage_mask =
		((1u << FIELD_GET(MMMU_DEBUG_INFO_BIGPAGE_WIDTH_MASK, u)) - 1u) << MMU_PAGE_SHIFT;
	mmu->superpage_mask =
		((1u << FIELD_GET(MMMU_DEBUG_INFO_SUPERPAGE_WIDTH_MASK, u)) - 1u) << MMU_PAGE_SHIFT;
	bypass_shift = (u & MMMU_DEBUG_INFO_BYPASS_4M) ?
		HUGEPAGE_SHIFT : ADDR_CAP_SHIFT;

	/* Disable MMU and clear sticky flags; meanwhile flush the TLB */
	MMU_WR(MMMU_CTRL_OFFSET,
	       MMMU_CTRL_CAP_EXCEEDED    |
	       MMMU_CTRL_PT_INVALID      |
	       MMMU_CTRL_WRITE_VIOLATION |
	       MMMU_CTRL_STATS_CLEAR     |
	       MMMU_CTRL_TLB_CLEAR);

	/*
	 * Put MMU into 2-level mode; set address cap and "bypass" range
	 * (note that some of these registers have unintuitive off-by-ones).
	 * Addresses below APERTURE_BASE are passed unchanged: this is
	 * useful for blocks which share an IOMMU with other blocks
	 * whose drivers are not IOMMU-aware.
	 */
	MMU_WR(MMMU_MISC_OFFSET,
	       MMU_RD(MMMU_MISC_OFFSET) & ~MMMU_MISC_SINGLE_TABLE);
	MMU_WR(MMMU_ADDR_CAP_OFFSET,
	       MMMU_ADDR_CAP_ENABLE +
	       (APERTURE_TOP >> ADDR_CAP_SHIFT) - 1);
	if (APERTURE_BASE > 0) {
		MMU_WR(MMMU_BYPASS_START_OFFSET,
		       MMMU_BYPASS_START_ENABLE + MMMU_BYPASS_START_INVERT +
		       (APERTURE_BASE >> bypass_shift) - 1);
		MMU_WR(MMMU_BYPASS_END_OFFSET,
		       MMMU_BYPASS_END_ENABLE +
		       (APERTURE_TOP >> bypass_shift));
	} else {
		MMU_WR(MMMU_BYPASS_START_OFFSET, 0);
		MMU_WR(MMMU_BYPASS_END_OFFSET, 0);
	}

	/* Ensure tables are zeroed (which marks all pages as invalid) */
	dma_sync_sgtable_for_cpu(mmu->dev, mmu->sgt, DMA_TO_DEVICE);
	memset(mmu->tables, 0, TABLES_ALLOC_SIZE);
	mmu->nmapped_pages = 0;

	/* Initialize the high-level table to point to the low-level pages */
	__sg_page_iter_start(&it.base, mmu->sgt->sgl, mmu->sgt->nents, 0);
	for (i = 0; i < L2_PAGES; i++) {
		if (!(i % (PAGE_SIZE / MMU_PAGE_SIZE))) {
			__sg_page_iter_dma_next(&it);
			u = (sg_page_iter_dma_address(&it) >> MMU_PAGE_SHIFT);
		} else {
			u++;
		}
		mmu->tables[TRANSLATED_PAGES + i] = MMMU_PTE_VALID + u;
	}

	/*
	 * Configure the addresses of the top-level table (offset because
	 * the aperture does not start from zero), and of the default page.
	 * For simplicity, both these regions are whole Linux pages.
	 */
	__sg_page_iter_dma_next(&it);
	u = (sg_page_iter_dma_address(&it) >> MMU_PAGE_SHIFT);
	MMU_WR(MMMU_PT_PA_BASE_OFFSET, u - (APERTURE_BASE >> L1_CHUNK_SHIFT));
	__sg_page_iter_dma_next(&it);
	u = (sg_page_iter_dma_address(&it) >> MMU_PAGE_SHIFT);
	MMU_WR(MMMU_ILLEGAL_ADR_OFFSET, MMMU_ILLEGAL_ADR_ENABLE + u);
	dma_sync_sgtable_for_device(mmu->dev, mmu->sgt, DMA_TO_DEVICE);
	mmu->dirty = false;

	/* Flush (and enable) the shared TLB cache; enable this MMU. */
	if (mmu->cache)
		bcm2712_iommu_cache_flush(mmu->cache);
	MMU_WR(MMMU_CTRL_OFFSET,
	       MMMU_CTRL_CAP_EXCEEDED_ABORT_EN    |
	       MMMU_CTRL_PT_INVALID_ABORT_EN      |
	       MMMU_CTRL_WRITE_VIOLATION_ABORT_EN |
	       MMMU_CTRL_STATS_ENABLE             |
	       MMMU_CTRL_ENABLE);
}

static int bcm2712_iommu_attach_dev(struct iommu_domain *domain, struct device *dev)
{
	struct bcm2712_iommu *mmu = dev ? dev_iommu_priv_get(dev) : 0;
	struct bcm2712_iommu_domain *mydomain =
		container_of(domain, struct bcm2712_iommu_domain, base);

	dev_info(dev, "%s: MMU %s\n",
		 __func__, mmu ? dev_name(mmu->dev) : "");

	if (mmu) {
		mydomain->mmu = mmu;
		mmu->domain = mydomain;

		if (mmu->dma_iova_offset) {
			domain->geometry.aperture_start =
				mmu->dma_iova_offset + APERTURE_BASE;
			domain->geometry.aperture_end =
				mmu->dma_iova_offset + APERTURE_TOP - 1ul;
		}

		return 0;
	}
	return -EINVAL;
}

static int bcm2712_iommu_map(struct iommu_domain *domain, unsigned long iova,
			     phys_addr_t pa, size_t bytes, size_t count,
			     int prot, gfp_t gfp, size_t *mapped)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	u32 entry = MMMU_PTE_VALID | (pa >> MMU_PAGE_SHIFT);
	u32 align = (u32)(iova | pa | bytes);
	unsigned int p;
	unsigned long flags;

	/* Reject if not entirely within our aperture (should never happen) */
	bytes *= count;
	if (iova < mmu->dma_iova_offset + APERTURE_BASE ||
	    iova + bytes > mmu->dma_iova_offset + APERTURE_TOP) {
		dev_warn(mmu->dev, "%s: iova=0x%lx pa=0x%llx bytes=0x%lx OUT OF RANGE\n",
			 __func__, iova,
			 (unsigned long long)pa, (unsigned long)bytes);
		*mapped = 0;

		return -EINVAL;
	}

	/* large page and write enable flags */
	if (!(align & ((1 << HUGEPAGE_SHIFT) - 1)))
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 3);
	else if (!(align & mmu->superpage_mask) && mmu->superpage_mask)
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 2);
	else if (!(align &  mmu->bigpage_mask) && mmu->bigpage_mask)
		entry |= FIELD_PREP(MMMU_PTE_PAGESIZE_MASK, 1);
	if (prot & IOMMU_WRITE)
		entry |= MMMU_PTE_WRITEABLE;

	/* Ensure tables are cache-coherent with CPU */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	if (!mmu->dirty) {
		dma_sync_sgtable_for_cpu(mmu->dev, mmu->sgt, DMA_TO_DEVICE);
		mmu->dirty = true;
	}

	/* Make iova relative to table base */
	iova -= (mmu->dma_iova_offset + APERTURE_BASE);

	/* Iterate over table by smallest native IOMMU page size */
	for (p = iova >> MMU_PAGE_SHIFT;
	     p < (iova + bytes) >> MMU_PAGE_SHIFT; p++) {
		mmu->nmapped_pages += !(mmu->tables[p]);
		mmu->tables[p] = entry++;
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	*mapped = bytes;

	return 0;
}

static size_t bcm2712_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
				  size_t bytes, size_t count,
				  struct iommu_iotlb_gather *gather)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	unsigned long flags;
	unsigned int p;

	/* Reject if not entirely within our aperture (should never happen) */
	bytes *= count;
	if (iova < mmu->dma_iova_offset + APERTURE_BASE ||
	    iova + bytes > mmu->dma_iova_offset + APERTURE_TOP)
		return 0;

	/* Record just the lower and upper bounds in "gather" */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	iommu_iotlb_gather_add_range(gather, iova, bytes);

	/* Ensure tables are cache-coherent with CPU */
	if (!mmu->dirty) {
		dma_sync_sgtable_for_cpu(mmu->dev, mmu->sgt, DMA_TO_DEVICE);
		mmu->dirty = true;
	}

	/* Make iova relative to table base */
	iova -= (mmu->dma_iova_offset + APERTURE_BASE);

	/* Clear table entries, this marks the addresses as illegal */
	for (p = iova >> MMU_PAGE_SHIFT;
	     p < (iova + bytes) >> MMU_PAGE_SHIFT;
	     p++) {
		mmu->nmapped_pages -= !!(mmu->tables[p]);
		mmu->tables[p] = 0;
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return bytes;
}

static int bcm2712_iommu_sync_range(struct iommu_domain *domain,
				    unsigned long iova, size_t size)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	unsigned long flags, iova_end;
	unsigned int i, p4;

	if (!mmu || !mmu->dirty)
		return 0;

	/* Ensure tables are cleaned from CPU cache or write-buffer */
	spin_lock_irqsave(&mmu->hw_lock, flags);
	dma_sync_sgtable_for_device(mmu->dev, mmu->sgt, DMA_TO_DEVICE);
	mmu->dirty = false;

	/* Flush the shared TLB cache */
	if (mmu->cache)
		bcm2712_iommu_cache_flush(mmu->cache);

	/*
	 * When flushing a large range or when nothing needs to be kept,
	 * it's quicker to use the"TLB_CLEAR" flag. Otherwise, invalidate
	 * TLB entries in lines of 4 words each. Each flush/clear operation
	 * should complete almost instantaneously.
	 */
	iova -= mmu->dma_iova_offset;
	iova_end = min(APERTURE_TOP, iova + size);
	iova = max(APERTURE_BASE, iova);
	if (mmu->nmapped_pages == 0 || iova_end - iova >= APERTURE_SIZE / 8) {
		MMU_WR(MMMU_CTRL_OFFSET,
		       MMMU_CTRL_CAP_EXCEEDED_ABORT_EN    |
		       MMMU_CTRL_PT_INVALID_ABORT_EN      |
		       MMMU_CTRL_WRITE_VIOLATION_ABORT_EN |
		       MMMU_CTRL_TLB_CLEAR                |
		       MMMU_CTRL_STATS_ENABLE             |
		       MMMU_CTRL_ENABLE);
		for (i = 0; i < 1024; i++) {
			if (!(MMMU_CTRL_TLB_CLEARING & MMU_RD(MMMU_CTRL_OFFSET)))
				break;
			cpu_relax();
		}
	} else {
		for (p4 = iova >> (MMU_PAGE_SHIFT + 2);
		     p4 < (iova_end + 3 * MMU_PAGE_SIZE) >> (MMU_PAGE_SHIFT + 2);
		     p4++) {
			MMU_WR(MMMU_SHOOT_DOWN_OFFSET,
			       MMMU_SHOOT_DOWN_SHOOT + (p4 << 2));
			for (i = 0; i < 1024; i++) {
				if (!(MMMU_SHOOT_DOWN_SHOOTING & MMU_RD(MMMU_SHOOT_DOWN_OFFSET)))
					break;
				cpu_relax();
			}
		}
	}

	spin_unlock_irqrestore(&mmu->hw_lock, flags);
	return 0;
}

static void bcm2712_iommu_sync(struct iommu_domain *domain,
			       struct iommu_iotlb_gather *gather)
{
	if (gather->end)
		bcm2712_iommu_sync_range(domain, gather->start,
					 gather->end - gather->start + 1);
}

static void bcm2712_iommu_sync_all(struct iommu_domain *domain)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);

	if (mmu)
		bcm2712_iommu_sync_range(domain,
					 mmu->dma_iova_offset + APERTURE_BASE,
					 APERTURE_SIZE);
}

static phys_addr_t bcm2712_iommu_iova_to_phys(struct iommu_domain *domain, dma_addr_t iova)
{
	struct bcm2712_iommu *mmu = domain_to_mmu(domain);
	u32 p;

	iova -= mmu->dma_iova_offset;
	if (iova  >= APERTURE_BASE && iova < APERTURE_TOP) {
		unsigned long flags;
		phys_addr_t addr;

		spin_lock_irqsave(&mmu->hw_lock, flags);
		p = (iova - APERTURE_BASE) >> MMU_PAGE_SHIFT;
		p = mmu->tables[p] & 0x0FFFFFFFu;
		addr = (((phys_addr_t)p) << MMU_PAGE_SHIFT) +
			(iova & (MMU_PAGE_SIZE - 1u));

		spin_unlock_irqrestore(&mmu->hw_lock, flags);
		return addr;
	} else if (iova < APERTURE_BASE) {
		return (phys_addr_t)iova;
	} else {
		return (phys_addr_t)-EINVAL;
	}
}

static void bcm2712_iommu_domain_free(struct iommu_domain *domain)
{
	struct bcm2712_iommu_domain *mydomain =
		container_of(domain, struct bcm2712_iommu_domain, base);

	kfree(mydomain);
}

static const struct iommu_domain_ops bcm2712_iommu_domain_ops = {
	.attach_dev	 = bcm2712_iommu_attach_dev,
	.map_pages	 = bcm2712_iommu_map,
	.unmap_pages	 = bcm2712_iommu_unmap,
	.iotlb_sync      = bcm2712_iommu_sync,
	.iotlb_sync_map  = bcm2712_iommu_sync_range,
	.flush_iotlb_all = bcm2712_iommu_sync_all,
	.iova_to_phys	 = bcm2712_iommu_iova_to_phys,
	.free		 = bcm2712_iommu_domain_free,
};

static struct iommu_domain *bcm2712_iommu_domain_alloc(unsigned int type)
{
	struct bcm2712_iommu_domain *domain;

	if (type != IOMMU_DOMAIN_UNMANAGED && type != IOMMU_DOMAIN_DMA)
		return NULL;

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return NULL;

	domain->base.type = type;
	domain->base.ops  = &bcm2712_iommu_domain_ops;
	domain->base.geometry.aperture_start = APERTURE_BASE;
	domain->base.geometry.aperture_end   = APERTURE_TOP - 1ul;
	domain->base.geometry.force_aperture = true;
	return &domain->base;
}

static struct iommu_device *bcm2712_iommu_probe_device(struct device *dev)
{
	struct bcm2712_iommu *mmu;

	/*
	 * For reasons I don't fully understand, we need to try both
	 * cases (dev_iommu_priv_get() and platform_get_drvdata())
	 * in order to get both GPU and ISP-BE to probe successfully.
	 */
	mmu = dev_iommu_priv_get(dev);
	if (!mmu) {
		struct device_node *np;
		struct platform_device *pdev;

		/* Ignore devices that don't have an "iommus" property with exactly one phandle */
		if (!dev->of_node ||
		    of_property_count_elems_of_size(dev->of_node, "iommus", sizeof(phandle)) != 1)
			return ERR_PTR(-ENODEV);

		np = of_parse_phandle(dev->of_node, "iommus", 0);
		if (!np)
			return ERR_PTR(-EINVAL);

		pdev = of_find_device_by_node(np);
		of_node_put(np);
		if (pdev)
			mmu = platform_get_drvdata(pdev);

		if (!mmu)
			return ERR_PTR(-ENODEV);
	}

	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));
	dev_iommu_priv_set(dev, mmu);
	return &mmu->iommu;
}

static void bcm2712_iommu_release_device(struct device *dev)
{
	dev_iommu_priv_set(dev, NULL);
}

static struct iommu_group *bcm2712_iommu_device_group(struct device *dev)
{
	struct bcm2712_iommu *mmu = dev_iommu_priv_get(dev);

	if (!mmu || !mmu->group)
		return ERR_PTR(-EINVAL);

	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));
	return iommu_group_ref_get(mmu->group);
}

static int bcm2712_iommu_of_xlate(struct device *dev,
				  const struct of_phandle_args *args)
{
	struct platform_device *iommu_dev;
	struct bcm2712_iommu *mmu;

	iommu_dev = of_find_device_by_node(args->np);
	mmu = platform_get_drvdata(iommu_dev);
	dev_iommu_priv_set(dev, mmu);
	dev_info(dev, "%s: MMU %s\n", __func__, dev_name(mmu->dev));

	return 0;
}

static bool bcm2712_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	return false;
}

static const struct iommu_ops bcm2712_iommu_ops = {
	.capable        = bcm2712_iommu_capable,
	.domain_alloc	= bcm2712_iommu_domain_alloc,
	.probe_device	= bcm2712_iommu_probe_device,
	.release_device	= bcm2712_iommu_release_device,
	.device_group	= bcm2712_iommu_device_group,
	/* Advertise native page sizes as well as 2M, 16K which Linux may prefer */
	.pgsize_bitmap	= (SZ_4M | SZ_2M | SZ_1M | SZ_64K | SZ_16K | SZ_4K),
	.default_domain_ops = &bcm2712_iommu_domain_ops,
	.of_xlate = bcm2712_iommu_of_xlate,
};

static int bcm2712_iommu_probe(struct platform_device *pdev)
{
	struct bcm2712_iommu *mmu;
	struct bcm2712_iommu_cache *cache = NULL;
	int ret;

	/* First of all, check for an IOMMU shared cache */
	if (pdev->dev.of_node) {
		struct device_node *cache_np;
		struct platform_device *cache_pdev;

		cache_np = of_parse_phandle(pdev->dev.of_node, "cache", 0);
		if (cache_np) {
			cache_pdev = of_find_device_by_node(cache_np);
			of_node_put(cache_np);
			if (cache_pdev && !IS_ERR(cache_pdev))
				cache = platform_get_drvdata(cache_pdev);
			if (!cache)
				return -EPROBE_DEFER;
		}
	}

	/* Allocate private data */
	mmu = devm_kzalloc(&pdev->dev, sizeof(*mmu), GFP_KERNEL);
	if (!mmu)
		return -ENOMEM;

	mmu->name = dev_name(&pdev->dev);
	mmu->dev = &pdev->dev;
	mmu->cache = cache;
	platform_set_drvdata(pdev, mmu);
	spin_lock_init(&mmu->hw_lock);

	/*
	 * XXX When an IOMMU is downstream of a PCIe RC or some other chip/bus
	 * and serves some of the masters thereon (others using pass-through),
	 * we seem to fumble and lose the "dma-ranges" address offset for
	 * masters using IOMMU. This property restores it, where needed.
	 */
	if (!pdev->dev.of_node ||
	    of_property_read_u64(pdev->dev.of_node, "dma-iova-offset",
				 &mmu->dma_iova_offset))
		mmu->dma_iova_offset = 0;

	/*
	 * The IOMMU is itself a device that allocates DMA-able memory
	 * to hold its translation tables. Provided the IOVA aperture
	 * is no larger than 4 GBytes (so that the L1 table fits within
	 * a single 4K page), we don't need the tables to be contiguous.
	 * Assume we can address at least 36 bits (64 GB).
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(36));
	WARN_ON(ret);
	mmu->sgt = dma_alloc_noncontiguous(&pdev->dev, TABLES_ALLOC_SIZE,
					   DMA_TO_DEVICE, GFP_KERNEL,
					   DMA_ATTR_ALLOC_SINGLE_PAGES);
	if (!mmu->sgt) {
		ret = -ENOMEM;
		goto done_err;
	}
	mmu->tables = dma_vmap_noncontiguous(&pdev->dev, TABLES_ALLOC_SIZE,
					     mmu->sgt);
	if (!mmu->tables) {
		ret = -ENOMEM;
		goto done_err;
	}

	/* Get IOMMU registers */
	mmu->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmu->reg_base)) {
		dev_err(&pdev->dev, "Failed to get IOMMU registers address\n");
		ret = PTR_ERR(mmu->reg_base);
		goto done_err;
	}

	/* Stuff */
	mmu->group = iommu_group_alloc();
	if (IS_ERR(mmu->group)) {
		ret = PTR_ERR(mmu->group);
		mmu->group = NULL;
		goto done_err;
	}
	ret = iommu_device_sysfs_add(&mmu->iommu, mmu->dev, NULL, mmu->name);
	if (ret)
		goto done_err;

	/* Initialize table and hardware */
	bcm2712_iommu_init(mmu);
	ret = iommu_device_register(&mmu->iommu, &bcm2712_iommu_ops, &pdev->dev);

	dev_info(&pdev->dev, "%s: Success\n", __func__);
	return 0;

done_err:
	dev_info(&pdev->dev, "%s: Failure %d\n", __func__, ret);
	if (mmu->group)
		iommu_group_put(mmu->group);
	if (mmu->tables)
		dma_vunmap_noncontiguous(&pdev->dev,
					 (void *)(mmu->tables));
	mmu->tables = NULL;
	if (mmu->sgt)
		dma_free_noncontiguous(&pdev->dev, TABLES_ALLOC_SIZE,
				       mmu->sgt, DMA_TO_DEVICE);
	mmu->sgt = NULL;
	kfree(mmu);
	return ret;
}

static void bcm2712_iommu_remove(struct platform_device *pdev)
{
	struct bcm2712_iommu *mmu = platform_get_drvdata(pdev);

	if (mmu->reg_base)
		MMU_WR(MMMU_CTRL_OFFSET, 0); /* disable the MMU */
	if (mmu->sgt)
		dma_free_noncontiguous(&pdev->dev, TABLES_ALLOC_SIZE,
				       mmu->sgt, DMA_TO_DEVICE);
}

static const struct of_device_id bcm2712_iommu_of_match[] = {
	{
		. compatible = "brcm,bcm2712-iommu"
	},
	{ /* sentinel */ },
};

static struct platform_driver bcm2712_iommu_driver = {
	.probe = bcm2712_iommu_probe,
	.remove = bcm2712_iommu_remove,
	.driver = {
		.name = "bcm2712-iommu",
		.of_match_table = bcm2712_iommu_of_match
	},
};

builtin_platform_driver(bcm2712_iommu_driver);
