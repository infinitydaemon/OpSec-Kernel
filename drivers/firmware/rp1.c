// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2023-24 Raspberry Pi Ltd.
 *
 * Parts of this driver are based on:
 *  - raspberrypi.c, by Eric Anholt <eric@anholt.net>
 *    Copyright (C) 2015 Broadcom
 */

#include <linux/dma-mapping.h>
#include <linux/kref.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rp1-firmware.h>

#define RP1_MAILBOX_FIRMWARE		0

enum rp1_firmware_ops {
	MBOX_SUCCESS		= 0x0000,
	GET_FIRMWARE_VERSION	= 0x0001, // na -> 160-bit version
	GET_FEATURE		= 0x0002, // FOURCC -> op base (0 == unsupported), op count

	COMMON_COUNT
};

struct rp1_firmware {
	struct mbox_client cl;
	struct mbox_chan *chan;	/* The doorbell channel */
	uint32_t __iomem *buf;	/* The shared buffer */
	u32 buf_size;		/* The size of the shared buffer */
	struct completion c;

	struct kref consumers;
};

struct rp1_get_feature_resp {
	uint32_t op_base;
	uint32_t op_count;
};

static DEFINE_MUTEX(transaction_lock);

static const struct of_device_id rp1_firmware_of_match[] = {
	{ .compatible = "raspberrypi,rp1-firmware", },
	{},
};
MODULE_DEVICE_TABLE(of, rp1_firmware_of_match);

static void response_callback(struct mbox_client *cl, void *msg)
{
	struct rp1_firmware *fw = container_of(cl, struct rp1_firmware, cl);

	complete(&fw->c);
}

/*
 * Sends a request to the RP1 firmware and synchronously waits for the reply.
 * Returns zero or a positive count of response bytes on success, negative on
 * error.
 */

int rp1_firmware_message(struct rp1_firmware *fw, uint16_t op,
			 const void *data, unsigned int data_len,
			 void *resp, unsigned int resp_space)
{
	int ret;
	u32 rc;

	if (data_len + 4 > fw->buf_size)
		return -EINVAL;

	mutex_lock(&transaction_lock);

	memcpy_toio(&fw->buf[1], data, data_len);
	writel((op << 16) | data_len, fw->buf);

	reinit_completion(&fw->c);
	ret = mbox_send_message(fw->chan, NULL);
	if (ret >= 0) {
		if (wait_for_completion_timeout(&fw->c, HZ))
			ret = 0;
		else
			ret = -ETIMEDOUT;
	} else {
		dev_err(fw->cl.dev, "mbox_send_message returned %d\n", ret);
	}

	if (ret == 0) {
		rc = readl(fw->buf);
		if (rc & 0x80000000) {
			ret = (int32_t)rc;
		} else {
			ret = min(rc, resp_space);
			memcpy_fromio(resp, &fw->buf[1], ret);
		}
	}

	mutex_unlock(&transaction_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(rp1_firmware_message);

static void rp1_firmware_delete(struct kref *kref)
{
	struct rp1_firmware *fw = container_of(kref, struct rp1_firmware, consumers);

	mbox_free_channel(fw->chan);
	kfree(fw);
}

void rp1_firmware_put(struct rp1_firmware *fw)
{
	if (!IS_ERR_OR_NULL(fw))
		kref_put(&fw->consumers, rp1_firmware_delete);
}
EXPORT_SYMBOL_GPL(rp1_firmware_put);

int rp1_firmware_get_feature(struct rp1_firmware *fw, uint32_t fourcc,
			     uint32_t *op_base, uint32_t *op_count)
{
	struct rp1_get_feature_resp resp;
	int ret;

	memset(&resp, 0, sizeof(resp));
	ret = rp1_firmware_message(fw, GET_FEATURE,
				   &fourcc, sizeof(fourcc),
				   &resp, sizeof(resp));
	*op_base = resp.op_base;
	*op_count = resp.op_count;
	if (ret < 0)
		return ret;
	if (ret < sizeof(resp) || !resp.op_base)
		return -EOPNOTSUPP;
	return 0;
}
EXPORT_SYMBOL_GPL(rp1_firmware_get_feature);

static void devm_rp1_firmware_put(void *data)
{
	struct rp1_firmware *fw = data;

	rp1_firmware_put(fw);
}

/**
 * rp1_firmware_get - Get pointer to rp1_firmware structure.
 *
 * The reference to rp1_firmware has to be released with rp1_firmware_put().
 *
 * Returns an error pointer on failure.
 */
struct rp1_firmware *rp1_firmware_get(struct device_node *client)
{
	const char *match = rp1_firmware_of_match[0].compatible;
	struct platform_device *pdev;
	struct device_node *fwnode;
	struct rp1_firmware *fw = NULL;

	if (!client)
		return NULL;
	fwnode = of_parse_phandle(client, "firmware", 0);
	if (!fwnode)
		return NULL;
	if (!of_device_is_compatible(fwnode, match)) {
		of_node_put(fwnode);
		return ERR_PTR(-ENXIO);
	}

	pdev = of_find_device_by_node(fwnode);
	of_node_put(fwnode);

	if (!pdev)
		return ERR_PTR(-ENXIO);

	fw = platform_get_drvdata(pdev);
	if (IS_ERR_OR_NULL(fw))
		goto err_exit;

	if (!kref_get_unless_zero(&fw->consumers))
		goto err_exit;

	put_device(&pdev->dev);

	return fw;

err_exit:
	put_device(&pdev->dev);
	return fw;
}
EXPORT_SYMBOL_GPL(rp1_firmware_get);

/**
 * devm_rp1_firmware_get - Get pointer to rp1_firmware structure.
 * @firmware_node:    Pointer to the firmware Device Tree node.
 *
 * Returns NULL is the firmware device is not ready.
 */
struct rp1_firmware *devm_rp1_firmware_get(struct device *dev, struct device_node *client)
{
	struct rp1_firmware *fw;
	int ret;

	fw = rp1_firmware_get(client);
	if (IS_ERR_OR_NULL(fw))
		return fw;

	ret = devm_add_action_or_reset(dev, devm_rp1_firmware_put, fw);
	if (ret)
		return ERR_PTR(ret);

	return fw;
}
EXPORT_SYMBOL_GPL(devm_rp1_firmware_get);

static int rp1_firmware_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *shmem;
	struct rp1_firmware *fw;
	struct resource res;
	uint32_t version[5];
	int ret;

	shmem = of_parse_phandle(dev->of_node, "shmem", 0);
	if (!of_device_is_compatible(shmem, "raspberrypi,rp1-shmem")) {
		of_node_put(shmem);
		return -ENXIO;
	}

	ret = of_address_to_resource(shmem, 0, &res);
	of_node_put(shmem);
	if (ret) {
		dev_err(dev, "failed to get shared memory (%pOF) - %d\n", shmem, ret);
		return ret;
	}

	/*
	 * Memory will be freed by rp1_firmware_delete() once all users have
	 * released their firmware handles. Don't use devm_kzalloc() here.
	 */
	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;

	fw->buf_size = resource_size(&res);
	fw->buf = devm_ioremap(dev, res.start, fw->buf_size);
	if (!fw->buf) {
		dev_err(dev, "failed to ioremap shared memory\n");
		kfree(fw);
		return -EADDRNOTAVAIL;
	}

	fw->cl.dev = dev;
	fw->cl.rx_callback = response_callback;
	fw->cl.tx_block = false;

	fw->chan = mbox_request_channel(&fw->cl, RP1_MAILBOX_FIRMWARE);
	if (IS_ERR(fw->chan)) {
		int ret = PTR_ERR(fw->chan);

		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get mbox channel: %d\n", ret);
		kfree(fw);
		return ret;
	}

	init_completion(&fw->c);
	kref_init(&fw->consumers);

	ret = rp1_firmware_message(fw, GET_FIRMWARE_VERSION,
				   NULL, 0, &version, sizeof(version));
	if (ret == sizeof(version)) {
		dev_info(dev, "RP1 Firmware version %08x%08x%08x%08x%08x\n",
			 version[0], version[1], version[2], version[3], version[4]);
		platform_set_drvdata(pdev, fw);
	} else {
		rp1_firmware_put(fw);
		platform_set_drvdata(pdev, ERR_PTR(-ENOENT));
	}

	return 0;
}

static void rp1_firmware_remove(struct platform_device *pdev)
{
	struct rp1_firmware *fw = platform_get_drvdata(pdev);

	rp1_firmware_put(fw);
}

static struct platform_driver rp1_firmware_driver = {
	.driver = {
		.name = "rp1-firmware",
		.of_match_table = rp1_firmware_of_match,
	},
	.probe		= rp1_firmware_probe,
	.remove		= rp1_firmware_remove,
};

module_platform_driver(rp1_firmware_driver);

MODULE_AUTHOR("Phil Elwell <phil@raspberrypi.com>");
MODULE_DESCRIPTION("RP1 firmware driver");
MODULE_LICENSE("GPL v2");
