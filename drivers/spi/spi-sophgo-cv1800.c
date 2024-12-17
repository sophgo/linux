// SPDX-License-Identifier: GPL-2.0
//
// Sophgo SPI NOR controller driver
//
// Copyright (C) 2020 Jingbao Qiu <qiujingbao.dlmu@gmail.com>

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#define SOPHGO_SPI_CTRL                 0x000
#define SOPHGO_SPI_CE_CTRL              0x004
#define SOPHGO_SPI_DLY_CTRL             0x008
#define SOPHGO_SPI_DMMR                 0x00C
#define SOPHGO_SPI_TRAN_CSR             0x010
#define SOPHGO_SPI_TRAN_NUM             0x014
#define SOPHGO_SPI_FIFO_PORT            0x018
#define SOPHGO_SPI_FIFO_PT              0x020
#define SOPHGO_SPI_INT_STS              0x028

#define SOPHGO_NOR_CTRL_SCK_DIV_MASK    GENMASK(10, 0)
#define SOPHGO_NOR_CTRL_DEFAULT_DIV     4
#define SOPHGO_NOR_DLY_CTRL_NEG_SAMPLE  BIT(14)

#define SOPHGO_NOR_CE_MANUAL            BIT(0)
#define SOPHGO_NOR_CE_MANUAL_EN         BIT(1)
#define SOPHGO_NOR_CE_ENABLE            (SOPHGO_NOR_CE_MANUAL | SOPHGO_NOR_CE_MANUAL_EN)
#define SOPHGO_NOR_CE_DISABLE           SOPHGO_NOR_CE_MANUAL_EN
#define SOPHGO_NOR_CE_HARDWARE          0

#define SOPHGO_NOR_TRAN_MODE_RX         BIT(0)
#define SOPHGO_NOR_TRAN_MODE_TX         BIT(1)
#define SOPHGO_NOR_TRAN_MODE_MASK       GENMASK(1, 0)
#define SOPHGO_NOR_TRANS_FAST           BIT(3)
#define SOPHGO_NOR_TRANS_BUS_WIDTH(n)   (n << 4)
#define SOPHGO_NOR_TRANS_BUS_WIDTH_MASK GENMASK(5, 4)

#define SOPHGO_NOR_TRANS_MIOS           BIT(7)

#define SOPHGO_NOR_TRAN_ADDR(n)         (n << 8)
#define SOPHGO_NOR_TRANS_ADDR_MASK      GENMASK(10, 8)
#define SOPHGO_NOR_TRANS_CMD            BIT(11)
#define SOPHGO_NOR_TRAN_FIFO_MASK       GENMASK(13, 12)
#define SOPHGO_NOR_TRAN_FIFO_8_BYTE     GENMASK(13, 12)
#define SOPHGO_NOR_TRAN_GO_BUSY         BIT(15)

#define SOPHGO_NOR_TRANS_DMMR_EN        BIT(20)
#define SOPHGO_NOR_TRANS_DMMR_CMD       BIT(21)

#define SOPHGO_NOR_TRANS_MMIO									\
	(SOPHGO_NOR_TRANS_FAST | SOPHGO_NOR_TRANS_DMMR_EN |			\
		SOPHGO_NOR_TRANS_DMMR_CMD | SOPHGO_NOR_TRANS_MIOS |		\
		SOPHGO_NOR_TRAN_MODE_RX | SOPHGO_NOR_TRAN_FIFO_8_BYTE)

#define SOPHGO_NOR_TRANS_PORT								\
	(SOPHGO_NOR_TRAN_MODE_MASK | SOPHGO_NOR_TRANS_ADDR_MASK |	\
		SOPHGO_NOR_TRAN_FIFO_MASK | SOPHGO_NOR_TRANS_BUS_WIDTH_MASK |	\
		SOPHGO_NOR_TRANS_BUS_WIDTH_MASK)

#define SOPHGO_NOR_FIFO_CAPACITY  8
#define SOPHGO_NOR_FIFO_AVAI_MASK GENMASK(3, 0)

#define SOPHGO_NOR_INT_TRAN_DONE  BIT(0)
#define SOPHGO_NOR_INT_RD_FIFO    BIT(1)
#define SOPHGO_NOR_INT_WR_FIFO    BIT(2)

struct sophgo_nor {
	struct spi_controller *ctlr;
	struct device *dev;
	void __iomem *io_base;
	uint32_t tran_csr_orig;
	uint32_t sck_div_orig;
	struct mutex lock;
};

static uint32_t sophgo_nor_clk_setup(struct sophgo_nor *spif, uint32_t sck_div)
{
	uint32_t reg;
	uint32_t old_clk;

	reg = readl(spif->io_base + SOPHGO_SPI_DLY_CTRL);

	if (sck_div < SOPHGO_NOR_CTRL_DEFAULT_DIV)
		reg |= SOPHGO_NOR_DLY_CTRL_NEG_SAMPLE;

	writel(reg, spif->io_base + SOPHGO_SPI_DLY_CTRL);

	reg = readl(spif->io_base + SOPHGO_SPI_CTRL);
	old_clk = FIELD_GET(SOPHGO_NOR_CTRL_SCK_DIV_MASK, reg);

	reg &= ~SOPHGO_NOR_CTRL_SCK_DIV_MASK;
	reg |= sck_div;
	writel(reg, spif->io_base + SOPHGO_SPI_CTRL);

	return old_clk;
}

static inline uint32_t sophgo_nor_trans_csr_config(struct sophgo_nor *spif,
					       const struct spi_mem_op *op)
{
	uint32_t tran_csr = 0;

	if (op->dummy.nbytes)
		tran_csr |= (op->dummy.nbytes * 8) / op->dummy.buswidth << 16;

	tran_csr |= SOPHGO_NOR_TRANS_MMIO;
	tran_csr |= SOPHGO_NOR_TRANS_BUS_WIDTH(op->data.buswidth / 2);
	tran_csr |= SOPHGO_NOR_TRAN_ADDR(op->addr.nbytes);

	return tran_csr;
}

static void sophgo_nor_config_mmio(struct sophgo_nor *spif,
				   const struct spi_mem_op *op,
				   uint32_t enabled)
{
	uint32_t ctrl, tran_csr;

	if (enabled) {
		spif->tran_csr_orig =
			readl(spif->io_base + SOPHGO_SPI_TRAN_CSR);
		tran_csr = sophgo_nor_trans_csr_config(spif, op);
		ctrl = SOPHGO_NOR_CE_HARDWARE;
	} else {
		tran_csr = spif->tran_csr_orig;
		ctrl = SOPHGO_NOR_CE_ENABLE;
	}

	writel(tran_csr, spif->io_base + SOPHGO_SPI_TRAN_CSR);
	writel(ctrl, spif->io_base + SOPHGO_SPI_CE_CTRL);
	writel(enabled, spif->io_base + SOPHGO_SPI_DMMR);
}

static void sophgo_nor_config_port(struct sophgo_nor *spif, uint32_t enabled)
{
	uint32_t ctrl = SOPHGO_NOR_CE_ENABLE;

	if (enabled) {
		ctrl = SOPHGO_NOR_CE_MANUAL_EN;
		writel(!enabled, spif->io_base + SOPHGO_SPI_DMMR);
	}

	writel(ctrl, spif->io_base + SOPHGO_SPI_CE_CTRL);
}

static int sophgo_nor_xfer(struct sophgo_nor *spif, const uint8_t *dout,
			   uint8_t *din, uint32_t data_bytes,
			   uint32_t bus_width)
{
	uint32_t xfer_size, off;
	uint32_t fifo_cnt;
	uint32_t interrupt_mask = 0;
	uint32_t stat, tran_csr = 0;
	int ret = 0;

	writel(0, spif->io_base + SOPHGO_SPI_INT_STS);
	writel(0, spif->io_base + SOPHGO_SPI_FIFO_PT);

	writew(data_bytes, spif->io_base + SOPHGO_SPI_TRAN_NUM);

	if (din && dout)
		return -1;
	else if (!din && !dout)
		return -1;

	tran_csr = readw(spif->io_base + SOPHGO_SPI_TRAN_CSR);

	tran_csr &= ~SOPHGO_NOR_TRANS_PORT;

	tran_csr |= SOPHGO_NOR_TRAN_FIFO_8_BYTE;
	tran_csr |= SOPHGO_NOR_TRAN_GO_BUSY;
	tran_csr |= (bus_width / 2) << 4;

	interrupt_mask |= SOPHGO_NOR_INT_TRAN_DONE;

	if (din) {
		tran_csr |= SOPHGO_NOR_TRAN_MODE_RX;
		interrupt_mask |= SOPHGO_NOR_INT_RD_FIFO;
		spif->sck_div_orig =
			sophgo_nor_clk_setup(spif, SOPHGO_NOR_CTRL_DEFAULT_DIV);
	} else if (dout) {
		tran_csr |= SOPHGO_NOR_TRAN_MODE_TX;
		interrupt_mask |= SOPHGO_NOR_INT_WR_FIFO;
	}

	writew(tran_csr, spif->io_base + SOPHGO_SPI_TRAN_CSR);

	ret = readb_poll_timeout(spif->io_base + SOPHGO_SPI_INT_STS, stat,
				 stat & interrupt_mask, 10, 30);
	if (ret)
		dev_warn(spif->dev, "%s stat timedout\n", __func__);

	off = 0;
	while (off < data_bytes) {
		xfer_size = min_t(uint32_t, data_bytes - off,
				  SOPHGO_NOR_FIFO_CAPACITY);

		fifo_cnt = readl(spif->io_base + SOPHGO_SPI_FIFO_PT) &
			   SOPHGO_NOR_FIFO_AVAI_MASK;

		if (fifo_cnt > SOPHGO_NOR_FIFO_CAPACITY)
			goto exit;

		if (din)
			xfer_size = min(xfer_size, fifo_cnt);
		else
			xfer_size = min_t(uint32_t, xfer_size,
					  SOPHGO_NOR_FIFO_CAPACITY - fifo_cnt);

		while (xfer_size--) {
			if (din)
				*(din + off) = readb(spif->io_base +
						     SOPHGO_SPI_FIFO_PORT);
			else
				writeb(*(dout + off),
				       spif->io_base + SOPHGO_SPI_FIFO_PORT);
			off++;
		}
	}

	ret = readb_poll_timeout(spif->io_base + SOPHGO_SPI_INT_STS, stat,
				 (stat & interrupt_mask), 10, 30);
	if (ret) {
		dev_warn(spif->dev, " %s command timed out %x\n", __func__,
			 stat);
	}

exit:
	writeb(0, spif->io_base + SOPHGO_SPI_FIFO_PT);
	stat = readb(spif->io_base + SOPHGO_SPI_INT_STS) & ~interrupt_mask;
	writeb(stat, spif->io_base + SOPHGO_SPI_INT_STS);

	if (din)
		sophgo_nor_clk_setup(spif, spif->sck_div_orig);

	return 0;
}

static int sophgo_nor_port_trans(struct sophgo_nor *spif,
				 const struct spi_mem_op *op)
{
	const uint8_t *dout = NULL;
	uint8_t *din = NULL;
	uint32_t addr;

	sophgo_nor_config_port(spif, 1);

	if (op->cmd.nbytes)
		sophgo_nor_xfer(spif, (uint8_t *)&op->cmd.opcode, NULL,
				op->cmd.nbytes, op->cmd.buswidth);

	if (op->addr.nbytes) {
		addr = cpu_to_be32(op->addr.val);
		sophgo_nor_xfer(spif, (uint8_t *)&addr, NULL, op->addr.nbytes,
				op->addr.buswidth);
	}

	if (op->data.dir == SPI_MEM_DATA_IN)
		din = op->data.buf.in;
	else if (op->data.dir == SPI_MEM_DATA_OUT)
		dout = op->data.buf.out;

	sophgo_nor_xfer(spif, dout, din, op->data.nbytes, op->data.buswidth);

	sophgo_nor_config_port(spif, 0);

	return 0;
}

static void sophgo_nore_read_mmio(struct sophgo_nor *spif,
				  const struct spi_mem_op *op)
{
	sophgo_nor_config_mmio(spif, op, 1);
	memcpy_fromio(op->data.buf.in, spif->io_base + op->addr.val,
		      op->data.nbytes);
	sophgo_nor_config_mmio(spif, op, 0);
}

static int sophgo_nor_exec_op(struct spi_mem *mem, const struct spi_mem_op *op)
{
	struct sophgo_nor *spif;

	spif = spi_controller_get_devdata(mem->spi->controller);

	mutex_lock(&spif->lock);
	if (op->data.dir == SPI_MEM_DATA_IN && op->data.nbytes &&
	    op->addr.nbytes == 4) {
		sophgo_nore_read_mmio(spif, op);
		goto exit;
	}

	sophgo_nor_port_trans(spif, op);

exit:
	mutex_unlock(&spif->lock);
	return 0;
}

static const struct spi_controller_mem_ops sophgo_nor_mem_ops = {
	.exec_op = sophgo_nor_exec_op,
};

static const struct of_device_id sophgo_nor_match[] = {
	{ .compatible = "sophgo,cv1800b-nor" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sophgo_nor_match);

static int sophgo_nor_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct sophgo_nor *sp;
	void __iomem *base;

	ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*sp));
	if (!ctlr)
		return -ENOMEM;

	sp = spi_controller_get_devdata(ctlr);
	dev_set_drvdata(&pdev->dev, ctlr);

	sp->dev = &pdev->dev;
	sp->ctlr = ctlr;

	sp->io_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ctlr->num_chipselect = 1;
	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->auto_runtime_pm = false;
	ctlr->mem_ops = &sophgo_nor_mem_ops;
	ctlr->mode_bits = SPI_RX_DUAL | SPI_TX_DUAL | SPI_RX_QUAD | SPI_TX_QUAD;

	mutex_init(&sp->lock);

	sophgo_nor_config_port(sp, 1);

	return devm_spi_register_controller(&pdev->dev, ctlr);
}

static void sophgo_nor_remove(struct platform_device *pdev)
{
	struct sophgo_nor *spif = platform_get_drvdata(pdev);

	mutex_destroy(&spif->lock);
}

static struct platform_driver sophgo_nor_driver = {
	.driver = {
		.name = "sophgo-spif",
		.of_match_table = sophgo_nor_match,
	},
	.probe = sophgo_nor_probe,
	.remove = sophgo_nor_remove,
};

module_platform_driver(sophgo_nor_driver);

MODULE_DESCRIPTION("Sophgo SPI NOR controller driver");
MODULE_AUTHOR("Jingbao Qiu <qiujingbao.dlmu@gmail.com>");
MODULE_LICENSE("GPL");
