/*
 * Driver for Broadcom BCM2835 SPI Controllers
 *
 * Copyright (C) 2012 Chris Boot
 * Copyright (C) 2013 Stephen Warren
 *
 * This driver is inspired by:
 * spi-ath79.c, Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * spi-atmel.c, Copyright (C) 2006 Atmel Corporation
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/spi/spi.h>

/* define some DEBUG pins */
#include "bcm2835-gpio-debugpin.h"
DEFINE_DEBUG_PIN()  /* used to mark "in worker thread"  */
DEFINE_DEBUG_PIN(2) /* used to mark "waiting on wakeup" */
DEFINE_DEBUG_PIN(3) /* used to mark "in SPI-interrupt"  */

/* SPI register offsets */
#define BCM2835_SPI_CS			0x00
#define BCM2835_SPI_FIFO		0x04
#define BCM2835_SPI_CLK			0x08
#define BCM2835_SPI_DLEN		0x0c
#define BCM2835_SPI_LTOH		0x10
#define BCM2835_SPI_DC			0x14

/* Bitfields in CS */
#define BCM2835_SPI_CS_LEN_LONG		0x02000000
#define BCM2835_SPI_CS_DMA_LEN		0x01000000
#define BCM2835_SPI_CS_CSPOL2		0x00800000
#define BCM2835_SPI_CS_CSPOL1		0x00400000
#define BCM2835_SPI_CS_CSPOL0		0x00200000
#define BCM2835_SPI_CS_RXF		0x00100000
#define BCM2835_SPI_CS_RXR		0x00080000
#define BCM2835_SPI_CS_TXD		0x00040000
#define BCM2835_SPI_CS_RXD		0x00020000
#define BCM2835_SPI_CS_DONE		0x00010000
#define BCM2835_SPI_CS_LEN		0x00002000
#define BCM2835_SPI_CS_REN		0x00001000
#define BCM2835_SPI_CS_ADCS		0x00000800
#define BCM2835_SPI_CS_INTR		0x00000400
#define BCM2835_SPI_CS_INTD		0x00000200
#define BCM2835_SPI_CS_DMAEN		0x00000100
#define BCM2835_SPI_CS_TA		0x00000080
#define BCM2835_SPI_CS_CSPOL		0x00000040
#define BCM2835_SPI_CS_CLEAR_RX		0x00000020
#define BCM2835_SPI_CS_CLEAR_TX		0x00000010
#define BCM2835_SPI_CS_CPOL		0x00000008
#define BCM2835_SPI_CS_CPHA		0x00000004
#define BCM2835_SPI_CS_CS_10		0x00000002
#define BCM2835_SPI_CS_CS_01		0x00000001

#define BCM2835_SPI_TIMEOUT_MS	30000
#define BCM2835_SPI_MODE_BITS	(SPI_CPOL | SPI_CPHA | SPI_CS_HIGH \
				| SPI_NO_CS | SPI_3WIRE)

/* the time we will poll the device */
#define BCM2835_SPI_POLLTIME_US 20

#define DRV_NAME	"spi-bcm2835"

struct bcm2835_spi {
	void __iomem *regs;
	struct clk *clk;
	int irq;
	struct completion done;
	const u8 *tx_buf;
	u8 *rx_buf;
	int len;
	u8 bits_per_word;
	spinlock_t cspol_lock;
	u32 cspol;
};

static inline u32 bcm2835_rd(struct bcm2835_spi *bs, unsigned reg)
{
	return readl(bs->regs + reg);
}

static inline void bcm2835_wr(struct bcm2835_spi *bs, unsigned reg, u32 val)
{
	writel(val, bs->regs + reg);
}

static inline void bcm2835_rd_fifo(struct bcm2835_spi *bs)
{
	u8 byte;

	while (bcm2835_rd(bs, BCM2835_SPI_CS) & BCM2835_SPI_CS_RXD) {
		byte = bcm2835_rd(bs, BCM2835_SPI_FIFO);
		if (bs->rx_buf)
			*bs->rx_buf++ = byte;
	}
}

static inline void bcm2835_wr_fifo(struct bcm2835_spi *bs)
{
	u32 val;

	while ( (bs->len)
		&& (bcm2835_rd(bs, BCM2835_SPI_CS) & BCM2835_SPI_CS_TXD)
		) {
		val = 0;
		if (bs->bits_per_word == 9) {
			if (bs->tx_buf) {
				val = *(const u16 *)bs->tx_buf;
				bs->tx_buf += 2;
			}
			bs->len-=2;
		} else {
			if (bs->tx_buf) {
				val = *bs->tx_buf++;
			}
			bs->len--;
		}
		bcm2835_wr(bs, BCM2835_SPI_FIFO, val);
	}
}

static irqreturn_t bcm2835_spi_interrupt(int irq, void *dev_id)
{
	struct spi_master *master = dev_id;
	struct bcm2835_spi *bs = spi_master_get_devdata(master);
	u32 cs = bcm2835_rd(bs, BCM2835_SPI_CS);
	debug_set_high3();

	/* Read as many bytes of data as possible */
	bcm2835_rd_fifo(bs);

	/* Write as many bytes of data as possible */
	bcm2835_wr_fifo(bs);

	/* if length is empty, then disable interrupts */
	if (! bs->len) {
		/* Disable SPI interrupts */
		cs &= ~(BCM2835_SPI_CS_INTR | BCM2835_SPI_CS_INTD);
		bcm2835_wr(bs, BCM2835_SPI_CS, cs);

		/*
		 * Wake up bcm2835_spi_transfer_one(), which will call
		 * bcm2835_spi_finish_transfer(), to drain the RX FIFO.
		 */
		complete(&bs->done);
	}

	debug_set_low3();
	return IRQ_HANDLED;
}

static int bcm2835_spi_start_transfer(struct spi_device *spi,
		struct spi_transfer *tfr)
{
	struct bcm2835_spi *bs = spi_master_get_devdata(spi->master);
	unsigned long spi_hz, clk_hz, cdiv,xfer_time_us;
	u32 cs = BCM2835_SPI_CS_TA;
	unsigned long flags;

	spi_hz = tfr->speed_hz;
	clk_hz = clk_get_rate(bs->clk);

	if (spi_hz >= clk_hz / 2) {
		cdiv = 2; /* clk_hz/2 is the fastest we can go */
	} else if (spi_hz) {
		/* CDIV must be a power of two */
		cdiv = DIV_ROUND_UP(clk_hz, spi_hz);
		/* make the divider "even" by rounding up
		 * this ensures that the phases are of equal length
		 */
		cdiv += (cdiv % 2) ;

		if (cdiv >= 65536)
			cdiv = 0; /* 0 is the slowest we can go */
	} else
		cdiv = 0; /* 0 is the slowest we can go */

	if (spi->mode & SPI_CPOL)
		cs |= BCM2835_SPI_CS_CPOL;
	if (spi->mode & SPI_CPHA)
		cs |= BCM2835_SPI_CS_CPHA;

	if (!(spi->mode & SPI_NO_CS)) {
		cs |= spi->chip_select;
	}

	spin_lock_irqsave(&bs->cspol_lock, flags);
	cs |= bs->cspol;
	spin_unlock_irqrestore(&bs->cspol_lock, flags);

	/* LoSSI/9-bit mode */
	if (spi->bits_per_word == 9)
		cs |= BCM2835_SPI_CS_LEN;

	/* 3-WIRE mode */
	if ( (spi->mode & SPI_3WIRE) && (tfr->rx_buf) )
		cs |= BCM2835_SPI_CS_REN;

	reinit_completion(&bs->done);
	bs->tx_buf = tfr->tx_buf;
	bs->rx_buf = tfr->rx_buf;
	bs->len = tfr->len;
	bs->bits_per_word = spi->bits_per_word;

        bcm2835_wr(bs, BCM2835_SPI_CLK, cdiv);
        /** Enable the HW block, but without the interrupts enabled,
         * so that we can fill in some data into the fifo now
         * and avoid delays doe to interrupt overheads...
         */
        bcm2835_wr(bs, BCM2835_SPI_CS, cs);
        /* Write as many bytes of data as possible */
        bcm2835_wr_fifo(bs);

	/* calculate how long we have to wait aproximately */
	xfer_time_us = cdiv
		* 9 /* 8bit + 1 clock gap */
		* tfr->len /* times the number of bytes to transfer */
		* 1000000 /* get the measure in us */
		/ clk_hz
		;

	/* if the time is bigger than the given BCM2835_SPI_POLLTIME_US
	 * or we still have bytes to transfer
	 * then run the interrupt
	 * note that this still hides the fact that the interrupt+wakeup
	 * is "expensive" and we should do all transfers in a message
	 * without waking up the worker thread
	 */
	if ((bs->len) || (xfer_time_us > BCM2835_SPI_POLLTIME_US))  {
		/* and now enable the interrupt for TX-empty*/
		bcm2835_wr(bs, BCM2835_SPI_CS,
			cs | BCM2835_SPI_CS_INTR | BCM2835_SPI_CS_INTD);
	} else {
		/* poll until we get there */
		while (!(bcm2835_rd(bs, BCM2835_SPI_CS)
				& BCM2835_SPI_CS_DONE)) ;
		/* and set completed */
		complete(&bs->done);
	}

	return 0;
}

static int bcm2835_spi_finish_transfer(struct spi_device *spi,
		struct spi_transfer *tfr, bool cs_change)
{
	struct bcm2835_spi *bs = spi_master_get_devdata(spi->master);
	u32 cs = bcm2835_rd(bs, BCM2835_SPI_CS);

	/* Drain RX FIFO */
	bcm2835_rd_fifo(bs);

	if (tfr->delay_usecs) {
		debug_set_high2();
		udelay(tfr->delay_usecs);
		debug_set_low2();
	}

	if (cs_change)
		/* Clear TA flag */
		bcm2835_wr(bs, BCM2835_SPI_CS, cs & ~BCM2835_SPI_CS_TA);

	return 0;
}

static int bcm2835_spi_transfer_one(struct spi_master *master,
		struct spi_message *mesg)
{
	struct bcm2835_spi *bs = spi_master_get_devdata(master);
	struct spi_transfer *tfr;
	struct spi_device *spi = mesg->spi;
	int err = 0;
	unsigned int timeout;
	bool cs_change;
	unsigned long flags;

	debug_set_high();

	list_for_each_entry(tfr, &mesg->transfers, transfer_list) {
		err = bcm2835_spi_start_transfer(spi, tfr);
		if (err)
			goto out;

		debug_set_high2();
		timeout = wait_for_completion_timeout(&bs->done,
				msecs_to_jiffies(BCM2835_SPI_TIMEOUT_MS));
		debug_set_low2();

		if (!timeout) {
			err = -ETIMEDOUT;
			goto out;
		}

		cs_change = tfr->cs_change ||
			list_is_last(&tfr->transfer_list, &mesg->transfers);

		err = bcm2835_spi_finish_transfer(spi, tfr, cs_change);
		if (err)
			goto out;

		mesg->actual_length += (tfr->len - bs->len);
	}

out:
	/* Clear FIFOs, and disable the HW block */
	spin_lock_irqsave(&bs->cspol_lock, flags);
	bcm2835_wr(bs, BCM2835_SPI_CS,
		BCM2835_SPI_CS_CLEAR_RX
		| BCM2835_SPI_CS_CLEAR_TX
		| bs->cspol );
	spin_unlock_irqrestore(&bs->cspol_lock, flags);

	mesg->status = err;
	spi_finalize_current_message(master);

	debug_set_low();
	return 0;
}

static int bcm2835_spi_setup(struct spi_device *spi)
{
	struct bcm2835_spi *bs = spi_master_get_devdata(spi->master);
	u32 mask = BCM2835_SPI_CS_CSPOL0 << spi->chip_select;
	unsigned long flags;

	spin_lock_irqsave(&bs->cspol_lock, flags);

	/* clear the bit */
	bs->cspol &= ~(mask);

	/* set cspol correctly */
	if (!(spi->mode & SPI_NO_CS)) {
		if (spi->mode & SPI_CS_HIGH)
			/* set the bit */
			bs->cspol |= mask;
	}

	spin_unlock_irqrestore(&bs->cspol_lock, flags);

	return 0;
}

static int bcm2835_spi_probe(struct platform_device *pdev)
{
	struct spi_master *master;
	struct bcm2835_spi *bs;
	struct resource *res;
	int err;

	debug_set_low();
	debug_set_low2();
	debug_set_low3();

	master = spi_alloc_master(&pdev->dev, sizeof(*bs));
	if (!master) {
		dev_err(&pdev->dev, "spi_alloc_master() failed\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, master);

	master->mode_bits = BCM2835_SPI_MODE_BITS;
	master->bits_per_word_mask = SPI_BPW_RANGE_MASK(8,9);
	master->num_chipselect = 3;
	master->transfer_one_message = bcm2835_spi_transfer_one;
	master->setup = bcm2835_spi_setup;
	master->dev.of_node = pdev->dev.of_node;
	master->rt = 1;

	bs = spi_master_get_devdata(master);

	init_completion(&bs->done);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	bs->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(bs->regs)) {
		err = PTR_ERR(bs->regs);
		goto out_master_put;
	}

	bs->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(bs->clk)) {
		err = PTR_ERR(bs->clk);
		dev_err(&pdev->dev, "could not get clk: %d\n", err);
		goto out_master_put;
	}

	bs->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (bs->irq <= 0) {
		dev_err(&pdev->dev, "could not get IRQ: %d\n", bs->irq);
		err = bs->irq ? bs->irq : -ENODEV;
		goto out_master_put;
	}

	spin_lock_init(&bs->cspol_lock);
	bs->cspol=0;

	clk_prepare_enable(bs->clk);

	err = devm_request_irq(&pdev->dev, bs->irq, bcm2835_spi_interrupt, 0,
				dev_name(&pdev->dev), master);
	if (err) {
		dev_err(&pdev->dev, "could not request IRQ: %d\n", err);
		goto out_clk_disable;
	}

	/* initialise the hardware */
	bcm2835_wr(bs, BCM2835_SPI_CS,
		bs->cspol
		| BCM2835_SPI_CS_CLEAR_RX
		| BCM2835_SPI_CS_CLEAR_TX);

	err = devm_spi_register_master(&pdev->dev, master);
	if (err) {
		dev_err(&pdev->dev, "could not register SPI master: %d\n", err);
		goto out_clk_disable;
	}

	return 0;

out_clk_disable:
	clk_disable_unprepare(bs->clk);
out_master_put:
	spi_master_put(master);
	return err;
}

static int bcm2835_spi_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct bcm2835_spi *bs = spi_master_get_devdata(master);

	/* Clear FIFOs, and disable the HW block */
	bcm2835_wr(bs, BCM2835_SPI_CS,
		   BCM2835_SPI_CS_CLEAR_RX | BCM2835_SPI_CS_CLEAR_TX);

	clk_disable_unprepare(bs->clk);

	return 0;
}

static const struct of_device_id bcm2835_spi_match[] = {
	{ .compatible = "brcm,bcm2835-spi", },
	{ .compatible = "brcm,bcm2708-spi", },
	{}
};
MODULE_DEVICE_TABLE(of, bcm2835_spi_match);

static struct platform_driver bcm2835_spi_driver = {
	.driver		= {
		.name		= DRV_NAME,
		.owner		= THIS_MODULE,
		.of_match_table	= bcm2835_spi_match,
	},
	.probe		= bcm2835_spi_probe,
	.remove		= bcm2835_spi_remove,
};
module_platform_driver(bcm2835_spi_driver);

MODULE_DESCRIPTION("SPI controller driver for Broadcom BCM2835");
MODULE_AUTHOR("Chris Boot <bootc@bootc.net>");
MODULE_LICENSE("GPL v2");
