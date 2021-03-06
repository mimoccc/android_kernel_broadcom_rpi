/*
 * BCM2835 DMA engine support
 *
 * This driver supports cyclic and scatter/gather DMA transfers.
 *
 * Author:      Florian Meier <florian.meier@koalo.de>
 *              Gellert Weisz <gellert@raspberrypi.org>
 *              Copyright 2013-2014
 *
 * Based on
 *	OMAP DMAengine support by Russell King
 *
 *	BCM2708 DMA Driver
 *	Copyright (C) 2010 Broadcom
 *
 *	Raspberry Pi PCM I2S ALSA Driver
 *	Copyright (c) by Phil Poole 2013
 *
 *	MARVELL MMP Peripheral DMA Driver
 *	Copyright 2012 Marvell International Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/spinlock.h>

#ifndef CONFIG_OF

/* dma manager */
#include <mach/dma.h>

//#define DMA_COMPLETE DMA_SUCCESS

#endif

#include <linux/of.h>
#include <linux/of_dma.h>

#include "virt-dma.h"


struct bcm2835_dmadev {
	struct dma_device ddev;
	spinlock_t lock;
	void __iomem *base;
	struct device_dma_parameters dma_parms;
};

struct bcm2835_dma_cb {
	uint32_t info;
	uint32_t src;
	uint32_t dst;
	uint32_t length;
	uint32_t stride;
	uint32_t next;
	uint32_t pad[2];
};

struct bcm2835_chan {
	struct virt_dma_chan vc;
	struct list_head node;

	struct dma_slave_config	cfg;
	bool cyclic;

	int ch;
	struct bcm2835_desc *desc;

	void __iomem *chan_base;
	int irq_number;

	unsigned int dreq;
};

struct bcm2835_desc {
	struct virt_dma_desc vd;
	enum dma_transfer_direction dir;

	unsigned int control_block_size;
	struct bcm2835_dma_cb *control_block_base;
	dma_addr_t control_block_base_phys;

	unsigned int frames;
	size_t size;
};

#define BCM2835_DMA_CS		0x00
#define BCM2835_DMA_ADDR	0x04
#define BCM2835_DMA_SOURCE_AD	0x0c
#define BCM2835_DMA_DEST_AD	0x10
#define BCM2835_DMA_NEXTCB	0x1C

/* DMA CS Control and Status bits */
#define BCM2835_DMA_ACTIVE	BIT(0)
#define BCM2835_DMA_INT	BIT(2)
#define BCM2835_DMA_ISPAUSED	BIT(4)  /* Pause requested or not active */
#define BCM2835_DMA_ISHELD	BIT(5)  /* Is held by DREQ flow control */
#define BCM2835_DMA_ERR	BIT(8)
#define BCM2835_DMA_ABORT	BIT(30) /* Stop current CB, go to next, WO */
#define BCM2835_DMA_RESET	BIT(31) /* WO, self clearing */

#define BCM2835_DMA_INT_EN	BIT(0)
#define BCM2835_DMA_WAIT_RESP	BIT(3)
#define BCM2835_DMA_D_INC	BIT(4)
#define BCM2835_DMA_D_WIDTH	BIT(5)
#define BCM2835_DMA_D_DREQ	BIT(6)
#define BCM2835_DMA_S_INC	BIT(8)
#define BCM2835_DMA_S_WIDTH	BIT(9)
#define BCM2835_DMA_S_DREQ	BIT(10)

#define BCM2835_DMA_PER_MAP(x)	((x) << 16)
#define	BCM2835_DMA_WAITS(x)	(((x)&0x1f) << 21)

#define SDHCI_BCM_DMA_WAITS 0  /* delays slowing DMA transfers: 0-31 */

#define BCM2835_DMA_DATA_TYPE_S8	1
#define BCM2835_DMA_DATA_TYPE_S16	2
#define BCM2835_DMA_DATA_TYPE_S32	4
#define BCM2835_DMA_DATA_TYPE_S128	16

#define BCM2835_DMA_BULK_MASK	BIT(0)
#define BCM2835_DMA_FIQ_MASK	(BIT(2) | BIT(3))


/* Valid only for channels 0 - 14, 15 has its own base address */
#define BCM2835_DMA_CHAN(n)	((n) << 8) /* Base address */
#define BCM2835_DMA_CHANIO(base, n) ((base) + BCM2835_DMA_CHAN(n))

#define MAX_LITE_TRANSFER 32768
#define MAX_NORMAL_TRANSFER 1073741824

static inline struct bcm2835_dmadev *to_bcm2835_dma_dev(struct dma_device *d)
{
	return container_of(d, struct bcm2835_dmadev, ddev);
}

static inline struct bcm2835_chan *to_bcm2835_dma_chan(struct dma_chan *c)
{
	return container_of(c, struct bcm2835_chan, vc.chan);
}

static inline struct bcm2835_desc *to_bcm2835_dma_desc(
		struct dma_async_tx_descriptor *t)
{
	return container_of(t, struct bcm2835_desc, vd.tx);
}

static void dma_dumpregs(struct bcm2835_chan *c)
{
	pr_debug("-------------DMA DUMPREGS-------------\n");
	pr_debug("CS=			%u\n",
		readl(c->chan_base + BCM2835_DMA_CS));
	pr_debug("ADDR=			%u\n",
		readl(c->chan_base + BCM2835_DMA_ADDR));
	pr_debug("SOURCE_ADDR=	%u\n",
		readl(c->chan_base + BCM2835_DMA_SOURCE_AD));
	pr_debug("DEST_AD=		%u\n",
		readl(c->chan_base + BCM2835_DMA_DEST_AD));
	pr_debug("NEXTCB=			%u\n",
		readl(c->chan_base + BCM2835_DMA_NEXTCB));
	pr_debug("--------------------------------------\n");
}

static void bcm2835_dma_desc_free(struct virt_dma_desc *vd)
{
	struct bcm2835_desc *desc = container_of(vd, struct bcm2835_desc, vd);
	dma_free_coherent(desc->vd.tx.chan->device->dev,
			desc->control_block_size,
			desc->control_block_base,
			desc->control_block_base_phys);
	kfree(desc);
}

static int bcm2835_dma_abort(void __iomem *chan_base)
{
	unsigned long cs;
	long int timeout = 10000;

	cs = readl(chan_base + BCM2835_DMA_CS);
	if (!(cs & BCM2835_DMA_ACTIVE))
		return 0;

	/* Write 0 to the active bit - Pause the DMA */
	writel(0, chan_base + BCM2835_DMA_CS);

	/* Wait for any current AXI transfer to complete */
	while ((cs & BCM2835_DMA_ISPAUSED) && --timeout) {
		cpu_relax();
		cs = readl(chan_base + BCM2835_DMA_CS);
	}

	/* We'll un-pause when we set of our next DMA */
	if (!timeout)
		return -ETIMEDOUT;

	if (!(cs & BCM2835_DMA_ACTIVE))
		return 0;

	/* Terminate the control block chain */
	writel(0, chan_base + BCM2835_DMA_NEXTCB);

	/* Abort the whole DMA */
	writel(BCM2835_DMA_ABORT | BCM2835_DMA_ACTIVE,
	       chan_base + BCM2835_DMA_CS);

	return 0;
}


static void bcm2835_dma_start_desc(struct bcm2835_chan *c)
{
	struct virt_dma_desc *vd = vchan_next_desc(&c->vc);
	struct bcm2835_desc *d;

	if (!vd) {
		c->desc = NULL;
		return;
	}

	list_del(&vd->node);

	c->desc = d = to_bcm2835_dma_desc(&vd->tx);

	writel(d->control_block_base_phys, c->chan_base + BCM2835_DMA_ADDR);
	writel(BCM2835_DMA_ACTIVE, c->chan_base + BCM2835_DMA_CS);

}

static irqreturn_t bcm2835_dma_callback(int irq, void *data)
{
	struct bcm2835_chan *c = data;
	struct bcm2835_desc *d;
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);

	/* Acknowledge interrupt */
	writel(BCM2835_DMA_INT, c->chan_base + BCM2835_DMA_CS);

	d = c->desc;

	if (d) {
		if (c->cyclic) {
			vchan_cyclic_callback(&d->vd);

			/* Keep the DMA engine running */
			writel(BCM2835_DMA_ACTIVE,
				c->chan_base + BCM2835_DMA_CS);

		} else {
			vchan_cookie_complete(&c->desc->vd);
			bcm2835_dma_start_desc(c);
		}
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return IRQ_HANDLED;
}

static int bcm2835_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	int ret;

	dev_dbg(c->vc.chan.device->dev,
			"Allocating DMA channel %d\n", c->ch);

	ret = request_irq(c->irq_number,
			bcm2835_dma_callback, 0, "DMA IRQ", c);

	return ret;
}

static void bcm2835_dma_free_chan_resources(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	vchan_free_chan_resources(&c->vc);
	free_irq(c->irq_number, c);

	dev_dbg(c->vc.chan.device->dev, "Freeing DMA channel %u\n", c->ch);
}

static size_t bcm2835_dma_desc_size(struct bcm2835_desc *d)
{
	return d->size;
}

static size_t bcm2835_dma_desc_size_pos(struct bcm2835_desc *d, dma_addr_t addr)
{
	unsigned int i;
	size_t size;

	for (size = i = 0; i < d->frames; i++) {
		struct bcm2835_dma_cb *control_block =
			&d->control_block_base[i];
		size_t this_size = control_block->length;
		dma_addr_t dma;

		if (d->dir == DMA_DEV_TO_MEM)
			dma = control_block->dst;
		else
			dma = control_block->src;

		if (size)
			size += this_size;
		else if (addr >= dma && addr < dma + this_size)
			size += dma + this_size - addr;
	}

	return size;
}

static enum dma_status bcm2835_dma_tx_status(struct dma_chan *chan,
	dma_cookie_t cookie, struct dma_tx_state *txstate)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	struct bcm2835_desc *d;
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;
	dma_addr_t pos;

	ret = dma_cookie_status(chan, cookie, txstate);
	if (ret == DMA_COMPLETE || !txstate)
		return ret;

	spin_lock_irqsave(&c->vc.lock, flags);
	vd = vchan_find_desc(&c->vc, cookie);
	if (vd) {
		txstate->residue =
			bcm2835_dma_desc_size(to_bcm2835_dma_desc(&vd->tx));
	} else if (c->desc && c->desc->vd.tx.cookie == cookie) {
		d = c->desc;

		if (d->dir == DMA_MEM_TO_DEV)
			pos = readl(c->chan_base + BCM2835_DMA_SOURCE_AD);
		else if (d->dir == DMA_DEV_TO_MEM)
			pos = readl(c->chan_base + BCM2835_DMA_DEST_AD);
		else
			pos = 0;

		txstate->residue = bcm2835_dma_desc_size_pos(d, pos);
	} else {
		txstate->residue = 0;
	}

	spin_unlock_irqrestore(&c->vc.lock, flags);

	return ret;
}

static void bcm2835_dma_issue_pending(struct dma_chan *chan)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (vchan_issue_pending(&c->vc) && !c->desc)
		bcm2835_dma_start_desc(c);

	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static struct dma_async_tx_descriptor *bcm2835_dma_prep_dma_cyclic(
	struct dma_chan *chan, dma_addr_t buf_addr, size_t buf_len,
	size_t period_len, enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	enum dma_slave_buswidth dev_width;
	struct bcm2835_desc *d;
	dma_addr_t dev_addr;
	unsigned int es, sync_type;
	unsigned int frame;

	/* Grab configuration */
	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev, "%s: bad direction?\n", __func__);
		return NULL;
	}

	if (direction == DMA_DEV_TO_MEM) {
		dev_addr = c->cfg.src_addr;
		dev_width = c->cfg.src_addr_width;
		sync_type = BCM2835_DMA_S_DREQ;
	} else {
		dev_addr = c->cfg.dst_addr;
		dev_width = c->cfg.dst_addr_width;
		sync_type = BCM2835_DMA_D_DREQ;
	}

	/* Bus width translates to the element size (ES) */
	switch (dev_width) {
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		es = BCM2835_DMA_DATA_TYPE_S32;
		break;
	default:
		return NULL;
	}

	/* Now allocate and setup the descriptor. */
	d = kzalloc(sizeof(*d), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->dir = direction;
	d->frames = buf_len / period_len;

	/* Allocate memory for control blocks */
	d->control_block_size = d->frames * sizeof(struct bcm2835_dma_cb);
	d->control_block_base = dma_zalloc_coherent(chan->device->dev,
			d->control_block_size, &d->control_block_base_phys,
			GFP_NOWAIT);

	if (!d->control_block_base) {
		kfree(d);
		return NULL;
	}

	/*
	 * Iterate over all frames, create a control block
	 * for each frame and link them together.
	 */
	for (frame = 0; frame < d->frames; frame++) {
		struct bcm2835_dma_cb *control_block =
			&d->control_block_base[frame];

		/* Setup adresses */
		if (d->dir == DMA_DEV_TO_MEM) {
			control_block->info = BCM2835_DMA_D_INC;
			control_block->src = dev_addr;
			control_block->dst = buf_addr + frame * period_len;
		} else {
			control_block->info = BCM2835_DMA_S_INC;
			control_block->src = buf_addr + frame * period_len;
			control_block->dst = dev_addr;
		}

		/* Enable interrupt */
		control_block->info |= BCM2835_DMA_INT_EN;

		/* Setup synchronization */
		if (sync_type != 0)
			control_block->info |= sync_type;

		/* Setup DREQ channel */
		if (c->cfg.slave_id != 0)
			control_block->info |=
				BCM2835_DMA_PER_MAP(c->cfg.slave_id);

		/* Length of a frame */
		control_block->length = period_len;
		d->size += control_block->length;

		/*
		 * Next block is the next frame.
		 * This function is called on cyclic DMA transfers.
		 * Therefore, wrap around at number of frames.
		 */
		control_block->next = d->control_block_base_phys +
			sizeof(struct bcm2835_dma_cb)
			* ((frame + 1) % d->frames);
	}

	c->cyclic = true;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}


static struct dma_async_tx_descriptor *bcm2835_dma_prep_slave_sg(
	struct dma_chan *chan, struct scatterlist *sgl,
	unsigned int sg_len, enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);
	enum dma_slave_buswidth dev_width;
	struct bcm2835_desc *d;
	dma_addr_t dev_addr;
	struct scatterlist *sgent;
	unsigned int es, sync_type;
	unsigned int i, j, splitct, max_size;

	if (!is_slave_direction(direction)) {
		dev_err(chan->device->dev, "%s: bad direction?\n", __func__);
		return NULL;
	}

	if (direction == DMA_DEV_TO_MEM) {
		dev_addr = c->cfg.src_addr;
		dev_width = c->cfg.src_addr_width;
		sync_type = BCM2835_DMA_S_DREQ;
	} else {
		dev_addr = c->cfg.dst_addr;
		dev_width = c->cfg.dst_addr_width;
		sync_type = BCM2835_DMA_D_DREQ;
	}

	/* Bus width translates to the element size (ES) */
	switch (dev_width) {
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		es = BCM2835_DMA_DATA_TYPE_S32;
		break;
	default:
		return NULL;
	}

	/* Now allocate and setup the descriptor. */
	d = kzalloc(sizeof(*d), GFP_NOWAIT);
	if (!d)
		return NULL;

	d->dir = direction;

	if (c->ch >= 8) /* we have a LITE channel */
		max_size = MAX_LITE_TRANSFER;
	else
		max_size = MAX_NORMAL_TRANSFER;

	/* We store the length of the SG list in d->frames
	   taking care to account for splitting up transfers
	   too large for a LITE channel */

	d->frames = 0;
	for_each_sg(sgl, sgent, sg_len, i) {
		uint32_t len = sg_dma_len(sgent);
		d->frames += 1 + len / max_size;
	}

	/* Allocate memory for control blocks */
	d->control_block_size = d->frames * sizeof(struct bcm2835_dma_cb);
	d->control_block_base = dma_zalloc_coherent(chan->device->dev,
			d->control_block_size, &d->control_block_base_phys,
			GFP_NOWAIT);

	if (!d->control_block_base) {
		kfree(d);
		return NULL;
	}

	/*
	 * Iterate over all SG entries, create a control block
	 * for each frame and link them together.
	 */

	/* we count the number of times an SG entry had to be splitct
	   as a result of using a LITE channel */
	splitct = 0;

	for_each_sg(sgl, sgent, sg_len, i) {
		dma_addr_t addr = sg_dma_address(sgent);
		uint32_t len = sg_dma_len(sgent);

		for (j = 0; j < len; j += max_size) {
			struct bcm2835_dma_cb *control_block =
				&d->control_block_base[i+splitct];

			/* Setup adresses */
			if (d->dir == DMA_DEV_TO_MEM) {
				control_block->info = BCM2835_DMA_D_INC |
					BCM2835_DMA_D_WIDTH | BCM2835_DMA_S_DREQ;
				control_block->src = dev_addr;
				control_block->dst = addr + (dma_addr_t)j;
			} else {
				control_block->info = BCM2835_DMA_S_INC |
					BCM2835_DMA_S_WIDTH | BCM2835_DMA_D_DREQ;
				control_block->src = addr + (dma_addr_t)j;
				control_block->dst = dev_addr;
			}

			/* Common part */
			control_block->info |= BCM2835_DMA_WAITS(SDHCI_BCM_DMA_WAITS);
			control_block->info |= BCM2835_DMA_WAIT_RESP;

			/* Enable  */
			if (i == sg_len-1 && len-j <= max_size)
				control_block->info |= BCM2835_DMA_INT_EN;

			/* Setup synchronization */
			if (sync_type != 0)
				control_block->info |= sync_type;

			/* Setup DREQ channel */
			c->dreq = c->cfg.slave_id; /* DREQ loaded from config */

			if (c->dreq != 0)
				control_block->info |=
					BCM2835_DMA_PER_MAP(c->dreq);

			/* Length of a frame */
			control_block->length = min(len-j, max_size);
			d->size += control_block->length;

			/*
			 * Next block is the next frame.
			 */
			if (i < sg_len-1 || len-j > max_size) {
				/* next block is the next frame. */
				control_block->next = d->control_block_base_phys +
				sizeof(struct bcm2835_dma_cb) * (i + splitct + 1);
			} else {
				/* next block is empty. */
				control_block->next = 0;
			}

			if (len-j > max_size)
				splitct++;
		}
	}

	c->cyclic = false;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static int bcm2835_dma_slave_config(struct bcm2835_chan *c,
		struct dma_slave_config *cfg)
{
	if ((cfg->direction == DMA_DEV_TO_MEM &&
	     cfg->src_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES) ||
	    (cfg->direction == DMA_MEM_TO_DEV &&
	     cfg->dst_addr_width != DMA_SLAVE_BUSWIDTH_4_BYTES) ||
	    !is_slave_direction(cfg->direction)) {
		return -EINVAL;
	}

	c->cfg = *cfg;

	return 0;
}

static int bcm2835_dma_terminate_all(struct bcm2835_chan *c)
{
	struct bcm2835_dmadev *d = to_bcm2835_dma_dev(c->vc.chan.device);
	unsigned long flags;
	int timeout = 10000;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->vc.lock, flags);

	/* Prevent this channel being scheduled */
	spin_lock(&d->lock);
	list_del_init(&c->node);
	spin_unlock(&d->lock);

	/*
	 * Stop DMA activity: we assume the callback will not be called
	 * after bcm_dma_abort() returns (even if it does, it will see
	 * c->desc is NULL and exit.)
	 */
	if (c->desc) {
		c->desc = NULL;
		bcm2835_dma_abort(c->chan_base);

		/* Wait for stopping */
		while (--timeout) {
			if (!(readl(c->chan_base + BCM2835_DMA_CS) &
						BCM2835_DMA_ACTIVE))
				break;

			cpu_relax();
		}

		if (!timeout)
			dev_err(d->ddev.dev, "DMA transfer could not be terminated\n");
	}

	vchan_get_all_descriptors(&c->vc, &head);
	spin_unlock_irqrestore(&c->vc.lock, flags);
	vchan_dma_desc_free_list(&c->vc, &head);

	return 0;
}

static int bcm2835_dma_control(struct dma_chan *chan, enum dma_ctrl_cmd cmd,
	unsigned long arg)
{
	struct bcm2835_chan *c = to_bcm2835_dma_chan(chan);

	switch (cmd) {
	case DMA_SLAVE_CONFIG:
		return bcm2835_dma_slave_config(c,
				(struct dma_slave_config *)arg);

	case DMA_TERMINATE_ALL:
		return bcm2835_dma_terminate_all(c);

	default:
		return -ENXIO;
	}
}

#ifdef CONFIG_OF
static int bcm2835_dma_chan_init(struct bcm2835_dmadev *d, int chan_id, int irq)
{
	struct bcm2835_chan *c;

	c = devm_kzalloc(d->ddev.dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->vc.desc_free = bcm2835_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);
	INIT_LIST_HEAD(&c->node);

	d->ddev.chancnt++;

	c->chan_base = BCM2835_DMA_CHANIO(d->base, chan_id);
	c->ch = chan_id;
	c->irq_number = irq;

	return 0;
}
#endif

static int bcm2708_dma_chan_init(struct bcm2835_dmadev *d,
	void __iomem *chan_base, int chan_id, int irq)
{
	struct bcm2835_chan *c;

	c = devm_kzalloc(d->ddev.dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->vc.desc_free = bcm2835_dma_desc_free;
	vchan_init(&c->vc, &d->ddev);
	INIT_LIST_HEAD(&c->node);

	d->ddev.chancnt++;

	c->chan_base = chan_base;
	c->ch = chan_id;
	c->irq_number = irq;

	return 0;
}


static void bcm2835_dma_free(struct bcm2835_dmadev *od)
{
	struct bcm2835_chan *c, *next;

	list_for_each_entry_safe(c, next, &od->ddev.channels,
				 vc.chan.device_node) {
		list_del(&c->vc.chan.device_node);
		tasklet_kill(&c->vc.task);
	}
}

static const struct of_device_id bcm2835_dma_of_match[] = {
	{ .compatible = "brcm,bcm2835-dma", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm2835_dma_of_match);

#ifdef CONFIG_OF
static struct dma_chan *bcm2835_dma_xlate(struct of_phandle_args *spec,
					   struct of_dma *ofdma)
{
	struct bcm2835_dmadev *d = ofdma->of_dma_data;
	struct dma_chan *chan;

	chan = dma_get_any_slave_channel(&d->ddev);
	if (!chan)
		return NULL;

	/* Set DREQ from param */
	to_bcm2835_dma_chan(chan)->dreq = spec->args[0];

	return chan;
}
#endif

static int bcm2835_dma_device_slave_caps(struct dma_chan *dchan,
	struct dma_slave_caps *caps)
{
	caps->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	caps->dstn_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	caps->directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	caps->cmd_pause = false;
	caps->cmd_terminate = true;

	return 0;
}

static int bcm2835_dma_probe(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od;
#ifdef CONFIG_OF
	struct resource *res;
	void __iomem *base;
	uint32_t chans_available;
#endif
	int rc;
	int i;
	int irq;


	if (!pdev->dev.dma_mask)
		pdev->dev.dma_mask = &pdev->dev.coherent_dma_mask;

	/* If CONFIG_OF is selected, device tree is used */
	/* hence the difference between probing */

#ifndef CONFIG_OF

	rc = dma_set_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;
	dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));


	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	pdev->dev.dma_parms = &od->dma_parms;
	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);


	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_PRIVATE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bcm2835_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bcm2835_dma_free_chan_resources;
	od->ddev.device_tx_status = bcm2835_dma_tx_status;
	od->ddev.device_issue_pending = bcm2835_dma_issue_pending;
	od->ddev.device_slave_caps = bcm2835_dma_device_slave_caps;
	od->ddev.device_prep_dma_cyclic = bcm2835_dma_prep_dma_cyclic;
	od->ddev.device_prep_slave_sg = bcm2835_dma_prep_slave_sg;
	od->ddev.device_control = bcm2835_dma_control;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);
	spin_lock_init(&od->lock);

	platform_set_drvdata(pdev, od);

	for (i = 0; i < 5; i++) {
		void __iomem *chan_base;
		int chan_id;

		chan_id = bcm_dma_chan_alloc(BCM_DMA_FEATURE_LITE,
			&chan_base,
			&irq);

		if (chan_id < 0)
			break;

		rc = bcm2708_dma_chan_init(od, chan_base, chan_id, irq);
		if (rc)
			goto err_no_dma;
	}
#else
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;


	od = devm_kzalloc(&pdev->dev, sizeof(*od), GFP_KERNEL);
	if (!od)
		return -ENOMEM;

	pdev->dev.dma_parms = &od->dma_parms;
	dma_set_max_seg_size(&pdev->dev, 0x3FFFFFFF);


	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	od->base = base;


	dma_cap_set(DMA_SLAVE, od->ddev.cap_mask);
	dma_cap_set(DMA_PRIVATE, od->ddev.cap_mask);
	dma_cap_set(DMA_CYCLIC, od->ddev.cap_mask);
	od->ddev.device_alloc_chan_resources = bcm2835_dma_alloc_chan_resources;
	od->ddev.device_free_chan_resources = bcm2835_dma_free_chan_resources;
	od->ddev.device_tx_status = bcm2835_dma_tx_status;
	od->ddev.device_issue_pending = bcm2835_dma_issue_pending;
	od->ddev.device_slave_caps = bcm2835_dma_device_slave_caps;
	od->ddev.device_prep_dma_cyclic = bcm2835_dma_prep_dma_cyclic;
	od->ddev.device_prep_slave_sg = bcm2835_dma_prep_slave_sg;
	od->ddev.device_control = bcm2835_dma_control;
	od->ddev.dev = &pdev->dev;
	INIT_LIST_HEAD(&od->ddev.channels);
	spin_lock_init(&od->lock);

	platform_set_drvdata(pdev, od);


	/* Request DMA channel mask from device tree */
	if (of_property_read_u32(pdev->dev.of_node,
			"brcm,dma-channel-mask",
			&chans_available)) {
		dev_err(&pdev->dev, "Failed to get channel mask\n");
		rc = -EINVAL;
		goto err_no_dma;
	}


	/*
	 * Do not use the FIQ and BULK channels,
	 * because they are used by the GPU.
	 */
	chans_available &= ~(BCM2835_DMA_FIQ_MASK | BCM2835_DMA_BULK_MASK);


	for (i = 0; i < pdev->num_resources; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			break;

		if (chans_available & (1 << i)) {
			rc = bcm2835_dma_chan_init(od, i, irq);
			if (rc)
				goto err_no_dma;
		}
	}

	dev_dbg(&pdev->dev, "Initialized %i DMA channels\n", i);

	/* Device-tree DMA controller registration */
	rc = of_dma_controller_register(pdev->dev.of_node,
			bcm2835_dma_xlate, od);
	if (rc) {
		dev_err(&pdev->dev, "Failed to register DMA controller\n");
		goto err_no_dma;
	}
#endif

	rc = dma_async_device_register(&od->ddev);
	if (rc) {
		dev_err(&pdev->dev,
			"Failed to register slave DMA engine device: %d\n", rc);
		goto err_no_dma;
	}

	dev_info(&pdev->dev, "Load BCM2835 DMA engine driver\n");

	return 0;

err_no_dma:
	bcm2835_dma_free(od);
	return rc;
}

static int bcm2835_dma_remove(struct platform_device *pdev)
{
	struct bcm2835_dmadev *od = platform_get_drvdata(pdev);

	dma_async_device_unregister(&od->ddev);
	bcm2835_dma_free(od);

	return 0;
}

#ifndef CONFIG_OF


static struct platform_driver bcm2835_dma_driver = {
	.probe	= bcm2835_dma_probe,
	.remove	= bcm2835_dma_remove,
	.driver = {
		.name = "bcm2708-dmaengine",
		.owner = THIS_MODULE,
	},
};

static struct platform_device *pdev;

static const struct platform_device_info bcm2835_dma_dev_info = {
	.name = "bcm2708-dmaengine",
	.id = -1,
};

static int bcm2835_dma_init(void)
{
	int rc = platform_driver_register(&bcm2835_dma_driver);

	if (rc == 0) {
		pdev = platform_device_register_full(&bcm2835_dma_dev_info);
		if (IS_ERR(pdev)) {
			platform_driver_unregister(&bcm2835_dma_driver);
			rc = PTR_ERR(pdev);
		}
	}

	return rc;
}
module_init(bcm2835_dma_init); /* preferable to subsys_initcall */

static void __exit bcm2835_dma_exit(void)
{
	platform_device_unregister(pdev);
	platform_driver_unregister(&bcm2835_dma_driver);
}
module_exit(bcm2835_dma_exit);

#else

static struct platform_driver bcm2835_dma_driver = {
	.probe	= bcm2835_dma_probe,
	.remove	= bcm2835_dma_remove,
	.driver = {
		.name = "bcm2835-dma",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(bcm2835_dma_of_match),
	},
};

module_platform_driver(bcm2835_dma_driver);

#endif

MODULE_ALIAS("platform:bcm2835-dma");
MODULE_DESCRIPTION("BCM2835 DMA engine driver");
MODULE_AUTHOR("Florian Meier <florian.meier@koalo.de>");
MODULE_AUTHOR("Gellert Weisz <gellert@raspberrypi.org>");
MODULE_LICENSE("GPL v2");
