/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/unaligned.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/fsi-sbefifo.h>
#include <linux/idr.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/fsi-occ.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#define CREATE_TRACE_POINTS
#include <trace/events/fsi_occ.h>

#define OCC_SRAM_BYTES		4096
#define OCC_CMD_DATA_BYTES	4090
#define OCC_RESP_DATA_BYTES	4089

/*
 * Assume we don't have FFDC, if we do we'll overflow and
 * fail the command. This needs to be big enough for simple
 * commands as well.
 */
#define OCC_SBE_STATUS_WORDS	16

#define OCC_TIMEOUT_MS		1000
#define OCC_CMD_IN_PRG_WAIT_MS	50

struct occ {
	struct device *sbefifo;
	char name[32];
	int idx;
	struct miscdevice mdev;
	struct list_head xfrs;
	spinlock_t list_lock;		/* lock access to the xfrs list */
	struct mutex occ_lock;		/* lock access to the hardware */
	struct work_struct work;
	bool cancel;
};

#define to_occ(x)	container_of((x), struct occ, mdev)

struct occ_response {
	u8 seq_no;
	u8 cmd_type;
	u8 return_status;
	__be16 data_length;
	u8 data[OCC_RESP_DATA_BYTES + 2];	/* two bytes checksum */
} __packed;

/*
 * transfer flags are NOT mutually exclusive
 *
 * Initial flags are none; transfer is created and queued from write(). All
 *  flags are cleared when the transfer is completed by closing the file or
 *  reading all of the available response data.
 * XFR_IN_PROGRESS is set when a transfer is started from occ_worker_putsram,
 *  and cleared if the transfer fails or occ_worker_getsram completes.
 * XFR_COMPLETE is set when a transfer fails or finishes occ_worker_getsram.
 * XFR_CANCELED is set when the transfer's client is released.
 */
enum {
	XFR_IN_PROGRESS,
	XFR_COMPLETE,
	XFR_CANCELED,
};

struct occ_xfr {
	struct list_head link;
	int rc;
	u8 buf[OCC_SRAM_BYTES];
	size_t cmd_data_length;
	size_t resp_data_length;
	unsigned long flags;
};

/*
 * client flags
 *
 * CLIENT_NONBLOCKING is set during open() if the file was opened with the
 *  O_NONBLOCK flag.
 * CLIENT_XFR_PENDING is set during write() and cleared when all data has been
 *  read.
 */
enum {
	CLIENT_NONBLOCKING,
	CLIENT_XFR_PENDING,
};

struct occ_client {
	struct kref kref;
	struct occ *occ;
	struct occ_xfr xfr;
	spinlock_t lock;		/* lock access to the client state */
	wait_queue_head_t wait;
	size_t read_offset;
	unsigned long flags;
};

#define to_client(x)	container_of((x), struct occ_client, xfr)

static struct workqueue_struct *occ_wq;

static DEFINE_IDA(occ_ida);

static int occ_enqueue_xfr(struct occ_xfr *xfr)
{
	int empty;
	unsigned long flags;
	struct occ_client *client = to_client(xfr);
	struct occ *occ = client->occ;

	if (occ->cancel)
		return -ENODEV;

	spin_lock_irqsave(&occ->list_lock, flags);

	empty = list_empty(&occ->xfrs);
	list_add_tail(&xfr->link, &occ->xfrs);

	spin_unlock_irqrestore(&occ->list_lock, flags);

	trace_occ_enq_xfer(client, xfr);

	if (empty)
		queue_work(occ_wq, &occ->work);

	return 0;
}

static void occ_get_client(struct occ_client *client)
{
	kref_get(&client->kref);
}

static void occ_client_release(struct kref *kref)
{
	struct occ_client *client = container_of(kref, struct occ_client,
						 kref);

	kfree(client);
}

static void occ_put_client(struct occ_client *client)
{
	kref_put(&client->kref, occ_client_release);
}

static struct occ_client *occ_open_common(struct occ *occ, unsigned long flags)
{
	struct occ_client *client = kzalloc(sizeof(*client), GFP_KERNEL);

	if (!client)
		return NULL;

	client->occ = occ;
	kref_init(&client->kref);
	spin_lock_init(&client->lock);
	init_waitqueue_head(&client->wait);

	if (flags & O_NONBLOCK)
		set_bit(CLIENT_NONBLOCKING, &client->flags);

	return client;
}

static int occ_open(struct inode *inode, struct file *file)
{
	struct occ_client *client;
	struct miscdevice *mdev = file->private_data;
	struct occ *occ = to_occ(mdev);

	client = occ_open_common(occ, file->f_flags);
	if (!client)
		return -ENOMEM;

	file->private_data = client;

	return 0;
}

static inline bool occ_read_ready(struct occ_xfr *xfr, struct occ *occ)
{
	return test_bit(XFR_COMPLETE, &xfr->flags) ||
		test_bit(XFR_CANCELED, &xfr->flags) || occ->cancel;
}

static ssize_t occ_read_common(struct occ_client *client, char __user *ubuf,
			       char *kbuf, size_t len)
{
	int rc;
	unsigned long flags;
	size_t bytes;
	struct occ_xfr *xfr;
	struct occ *occ;

	if (!client)
		return -ENODEV;

	if (len > OCC_SRAM_BYTES)
		return -EINVAL;

	occ_get_client(client);
	xfr = &client->xfr;
	occ = client->occ;

	spin_lock_irqsave(&client->lock, flags);

	if (!test_bit(CLIENT_XFR_PENDING, &client->flags)) {
		/* we just finished reading all data, return 0 */
		if (client->read_offset) {
			rc = 0;
			client->read_offset = 0;
		} else {
			rc = -ENOMSG;
		}

		goto done;
	}

	if (!test_bit(XFR_COMPLETE, &xfr->flags)) {
		if (test_bit(CLIENT_NONBLOCKING, &client->flags)) {
			rc = -EAGAIN;
			goto done;
		}

		spin_unlock_irqrestore(&client->lock, flags);

		rc = wait_event_interruptible(client->wait,
					      occ_read_ready(xfr, occ));

		spin_lock_irqsave(&client->lock, flags);

		if (!test_bit(XFR_COMPLETE, &xfr->flags)) {
			if (occ->cancel || test_bit(XFR_CANCELED, &xfr->flags))
				rc = -ENODEV;
			else
				rc = -EINTR;

			goto done;
		}
	}

	if (xfr->rc) {
		rc = xfr->rc;
		goto done;
	}

	bytes = min(len, xfr->resp_data_length - client->read_offset);
	if (ubuf) {
		if (copy_to_user(ubuf, &xfr->buf[client->read_offset],
				 bytes)) {
			rc = -EFAULT;
			goto done;
		}
	} else {
		memcpy(kbuf, &xfr->buf[client->read_offset], bytes);
	}

	client->read_offset += bytes;

	/* xfr done */
	if (client->read_offset == xfr->resp_data_length)
		clear_bit(CLIENT_XFR_PENDING, &client->flags);

	rc = bytes;

done:
	spin_unlock_irqrestore(&client->lock, flags);
	trace_occ_read_complete(client, xfr);
	occ_put_client(client);
	return rc;
}

static ssize_t occ_read(struct file *file, char __user *buf, size_t len,
			loff_t *offset)
{
	struct occ_client *client = file->private_data;

	return occ_read_common(client, buf, NULL, len);
}

static ssize_t occ_write_common(struct occ_client *client,
				const char __user *ubuf, const char *kbuf,
				size_t len)
{
	int rc;
	unsigned long flags;
	unsigned int i;
	u16 data_length, checksum = 0;
	struct occ_xfr *xfr;

	if (!client)
		return -ENODEV;

	if (len > (OCC_CMD_DATA_BYTES + 3) || len < 3)
		return -EINVAL;

	occ_get_client(client);
	xfr = &client->xfr;

	trace_occ_write_begin(client, xfr);
	spin_lock_irqsave(&client->lock, flags);

	if (test_bit(CLIENT_XFR_PENDING, &client->flags)) {
		rc = -EBUSY;
		goto done;
	}

	memset(xfr, 0, sizeof(*xfr));	/* clear out the transfer */
	xfr->buf[0] = 1;		/* occ sequence number */

	/*
	 * Assume user data follows the occ command format.
	 * byte 0: command type
	 * bytes 1-2: data length (msb first)
	 * bytes 3-n: data
	 */
	if (ubuf) {
		if (copy_from_user(&xfr->buf[1], ubuf, len)) {
			rc = -EFAULT;
			goto done;
		}
	} else {
		memcpy(&xfr->buf[1], kbuf, len);
	}

	data_length = (xfr->buf[2] << 8) + xfr->buf[3];
	if (data_length > OCC_CMD_DATA_BYTES) {
		rc = -EINVAL;
		goto done;
	}

	for (i = 0; i < data_length + 4; ++i)
		checksum += xfr->buf[i];

	xfr->buf[data_length + 4] = checksum >> 8;
	xfr->buf[data_length + 5] = checksum & 0xFF;

	xfr->cmd_data_length = data_length + 6;
	client->read_offset = 0;

	rc = occ_enqueue_xfr(xfr);
	if (rc)
		goto done;

	set_bit(CLIENT_XFR_PENDING, &client->flags);
	rc = len;

done:
	spin_unlock_irqrestore(&client->lock, flags);
	occ_put_client(client);
	return rc;
}

static ssize_t occ_write(struct file *file, const char __user *buf,
			 size_t len, loff_t *offset)
{
	struct occ_client *client = file->private_data;

	return occ_write_common(client, buf, NULL, len);
}

static int occ_release_common(struct occ_client *client)
{
	unsigned long flags;
	struct occ *occ;
	struct occ_xfr *xfr;

	if (!client)
		return -ENODEV;

	xfr = &client->xfr;
	occ = client->occ;

	spin_lock_irqsave(&client->lock, flags);

	set_bit(XFR_CANCELED, &xfr->flags);
	if (!test_bit(CLIENT_XFR_PENDING, &client->flags))
		goto done;

	spin_lock(&occ->list_lock);

	if (!test_bit(XFR_IN_PROGRESS, &xfr->flags)) {
		/* already deleted from list if complete */
		if (!test_bit(XFR_COMPLETE, &xfr->flags))
			list_del(&xfr->link);
	}

	spin_unlock(&occ->list_lock);

	wake_up_all(&client->wait);

done:
	spin_unlock_irqrestore(&client->lock, flags);

	occ_put_client(client);
	return 0;
}

static int occ_release(struct inode *inode, struct file *file)
{
	struct occ_client *client = file->private_data;

	return occ_release_common(client);
}

static const struct file_operations occ_fops = {
	.owner = THIS_MODULE,
	.open = occ_open,
	.read = occ_read,
	.write = occ_write,
	.release = occ_release,
};

static int occ_verify_checksum(struct occ_response *resp, u16 data_length)
{
	u16 i;
	u16 checksum;
	/* Fetch the two bytes after the data for the checksum. */
	u16 checksum_resp = get_unaligned_be16(&resp->data[data_length]);

	checksum = resp->seq_no;
	checksum += resp->cmd_type;
	checksum += resp->return_status;
	checksum += (data_length >> 8) + (data_length & 0xFF);

	for (i = 0; i < data_length; ++i)
		checksum += resp->data[i];

	if (checksum != checksum_resp)
		return -EBADMSG;

	return 0;
}

static int occ_getsram(struct device *sbefifo, u32 address, u8 *data,
		       ssize_t len)
{
	u32 data_len = ((len + 7) / 8) * 8;	/* must be multiples of 8 B */
	size_t resp_len, resp_data_len;
	__be32 *resp, cmd[5];
	int rc;

	/*
	 * Magic sequence to do SBE getsram command. SBE will fetch data from
	 * specified SRAM address.
	 */
	cmd[0] = cpu_to_be32(0x5);
	cmd[1] = cpu_to_be32(SBEFIFO_CMD_GET_OCC_SRAM);
	cmd[2] = cpu_to_be32(1);
	cmd[3] = cpu_to_be32(address);
	cmd[4] = cpu_to_be32(data_len);

	resp_len = (data_len >> 2) + OCC_SBE_STATUS_WORDS;
	resp = kzalloc(resp_len << 2 , GFP_KERNEL);
	if (!resp)
		return -ENOMEM;

	rc = sbefifo_submit(sbefifo, cmd, 5, resp, &resp_len);
	if (rc)
		goto free;
	rc = sbefifo_parse_status(sbefifo, SBEFIFO_CMD_GET_OCC_SRAM,
				  resp, resp_len, &resp_len);
	if (rc)
		goto free;

	resp_data_len = be32_to_cpu(resp[resp_len - 1]);
	if (resp_data_len != data_len) {
		pr_err("occ: SRAM read expected %d bytes got %zd\n",
		       data_len, resp_data_len);
		rc = -EBADMSG;
	} else {
		memcpy(data, resp, len);
	}

free:
	/* Convert positive SBEI status */
	if (rc > 0) {
		pr_err("occ: SRAM read returned failure status: %08x\n", rc);
		rc = -EBADMSG;
	}
	kfree(resp);
	return rc;
}

static int occ_putsram(struct device *sbefifo, u32 address, u8 *data,
		       ssize_t len)
{
	size_t cmd_len, buf_len, resp_len, resp_data_len;
	u32 data_len = ((len + 7) / 8) * 8;	/* must be multiples of 8 B */
	__be32 *buf;
	int rc;

	/*
	 * We use the same buffer for command and response, make
	 * sure it's big enough
	 */
	resp_len = OCC_SBE_STATUS_WORDS;
	cmd_len = (data_len >> 2) + 5;
	buf_len = max(cmd_len, resp_len);
	buf = kzalloc(buf_len << 2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/*
	 * Magic sequence to do SBE putsram command. SBE will transfer
	 * data to specified SRAM address.
	 */
	buf[0] = cpu_to_be32(cmd_len);
	buf[1] = cpu_to_be32(SBEFIFO_CMD_PUT_OCC_SRAM);
	buf[2] = cpu_to_be32(1);
	buf[3] = cpu_to_be32(address);
	buf[4] = cpu_to_be32(data_len);

	memcpy(&buf[5], data, len);

	rc = sbefifo_submit(sbefifo, buf, cmd_len, buf, &resp_len);
	if (rc)
		goto free;
	rc = sbefifo_parse_status(sbefifo, SBEFIFO_CMD_PUT_OCC_SRAM,
				  buf, resp_len, &resp_len);
	if (rc)
		goto free;

	if (resp_len != 1) {
		pr_err("occ: SRAM write response lenght invalid: %zd\n",
		       resp_len);
		rc = -EBADMSG;
	} else {
		resp_data_len = be32_to_cpu(buf[0]);
		if (resp_data_len != data_len) {
			pr_err("occ: SRAM write expected %d bytes got %zd\n",
			       data_len, resp_data_len);
			rc = -EBADMSG;
		}
	}
free:
	/* Convert positive SBEI status */
	if (rc > 0) {
		pr_err("occ: SRAM write returned failure status: %08x\n", rc);
		rc = -EBADMSG;
	}
	kfree(buf);
	return rc;
}

static int occ_trigger_attn(struct device *sbefifo)
{
	__be32 buf[OCC_SBE_STATUS_WORDS];
	size_t resp_len, resp_data_len;
	int rc;

	BUILD_BUG_ON(OCC_SBE_STATUS_WORDS < 7);
	resp_len = OCC_SBE_STATUS_WORDS;

	buf[0] = cpu_to_be32(0x5 + 0x2);        /* Chip-op length in words */
	buf[1] = cpu_to_be32(SBEFIFO_CMD_PUT_OCC_SRAM);
	buf[2] = cpu_to_be32(0x3);              /* Mode: Circular */
	buf[3] = cpu_to_be32(0x0);              /* Address: ignored in mode 3 */
	buf[4] = cpu_to_be32(0x8);              /* Data length in bytes */
	buf[5] = cpu_to_be32(0x20010000);       /* Trigger OCC attention */
	buf[6] = 0;

	rc = sbefifo_submit(sbefifo, buf, 7, buf, &resp_len);
	if (rc)
		goto error;
	rc = sbefifo_parse_status(sbefifo, SBEFIFO_CMD_PUT_OCC_SRAM,
				  buf, resp_len, &resp_len);
	if (rc)
		goto error;

	if (resp_len != 1) {
		pr_err("occ: SRAM attn response lenght invalid: %zd\n",
		       resp_len);
		rc = -EBADMSG;
	} else {
		resp_data_len = be32_to_cpu(buf[0]);
		if (resp_data_len != 8) {
			pr_err("occ: SRAM attn expected 8 bytes got %zd\n",
			       resp_data_len);
			rc = -EBADMSG;
		}
	}
 error:
	/* Convert positive SBEI status */
	if (rc > 0) {
		pr_err("occ: SRAM attn returned failure status: %08x\n", rc);
		rc = -EBADMSG;
	}


	return rc;
}

static void occ_worker(struct work_struct *work)
{
	int rc = 0, empty;
	u16 resp_data_length;
	unsigned long flags;
	unsigned long start;
	const unsigned long timeout = msecs_to_jiffies(OCC_TIMEOUT_MS);
	const long int wait_time = msecs_to_jiffies(OCC_CMD_IN_PRG_WAIT_MS);
	struct occ_xfr *xfr;
	struct occ_response *resp;
	struct occ_client *client;
	struct occ *occ = container_of(work, struct occ, work);
	struct device *sbefifo = occ->sbefifo;

again:
	if (occ->cancel)
		return;

	spin_lock_irqsave(&occ->list_lock, flags);

	xfr = list_first_entry_or_null(&occ->xfrs, struct occ_xfr, link);
	if (!xfr) {
		spin_unlock_irqrestore(&occ->list_lock, flags);
		return;
	}

	client = to_client(xfr);
	occ_get_client(client);
	resp = (struct occ_response *)xfr->buf;
	set_bit(XFR_IN_PROGRESS, &xfr->flags);

	spin_unlock_irqrestore(&occ->list_lock, flags);
	trace_occ_worker_xfer_begin(client, xfr);
	mutex_lock(&occ->occ_lock);

	start = jiffies;

	/* write occ command */
	rc = occ_putsram(sbefifo, 0xFFFBE000, xfr->buf,
			 xfr->cmd_data_length);
	if (rc)
		goto done;

	rc = occ_trigger_attn(sbefifo);
	if (rc)
		goto done;

	/* read occ response */
	do {
		rc = occ_getsram(sbefifo, 0xFFFBF000, xfr->buf, 8);
		if (rc)
			goto done;

		if (resp->return_status == OCC_RESP_CMD_IN_PRG) {
			rc = -EALREADY;

			if (time_after(jiffies, start + timeout))
				break;

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(wait_time);
		}
	} while (rc);

	resp_data_length = get_unaligned_be16(&resp->data_length);
	if (resp_data_length > OCC_RESP_DATA_BYTES) {
		rc = -EMSGSIZE;
		goto done;
	}

	if (resp_data_length > 1) {
		/* already got 3 bytes resp, also need 2 bytes checksum */
		rc = occ_getsram(sbefifo, 0xFFFBF008, &xfr->buf[8],
				 resp_data_length - 1);
		if (rc)
			goto done;
	}

	xfr->resp_data_length = resp_data_length + 7;

	rc = occ_verify_checksum(resp, resp_data_length);

done:
	mutex_unlock(&occ->occ_lock);

	xfr->rc = rc;
	set_bit(XFR_COMPLETE, &xfr->flags);

	spin_lock_irqsave(&occ->list_lock, flags);

	clear_bit(XFR_IN_PROGRESS, &xfr->flags);
	list_del(&xfr->link);
	empty = list_empty(&occ->xfrs);

	spin_unlock_irqrestore(&occ->list_lock, flags);

	wake_up_interruptible(&client->wait);
	trace_occ_worker_xfer_complete(client, xfr);
	occ_put_client(client);

	if (!empty)
		goto again;
}

struct occ_client *occ_drv_open(struct device *dev, unsigned long flags)
{
	struct occ *occ = dev_get_drvdata(dev);

	if (!occ)
		return NULL;

	return occ_open_common(occ, flags);
}
EXPORT_SYMBOL_GPL(occ_drv_open);

int occ_drv_read(struct occ_client *client, char *buf, size_t len)
{
	return occ_read_common(client, NULL, buf, len);
}
EXPORT_SYMBOL_GPL(occ_drv_read);

int occ_drv_write(struct occ_client *client, const char *buf, size_t len)
{
	return occ_write_common(client, NULL, buf, len);
}
EXPORT_SYMBOL_GPL(occ_drv_write);

void occ_drv_release(struct occ_client *client)
{
	occ_release_common(client);
}
EXPORT_SYMBOL_GPL(occ_drv_release);

static int occ_unregister_child(struct device *dev, void *data)
{
	struct platform_device *hwmon_dev = to_platform_device(dev);

	platform_device_unregister(hwmon_dev);

	return 0;
}

static int occ_probe(struct platform_device *pdev)
{
	int rc;
	u32 reg;
	struct occ *occ;
	struct platform_device *hwmon_dev;
	struct device *dev = &pdev->dev;
	struct platform_device_info hwmon_dev_info = {
		.parent = dev,
		.name = "occ-hwmon",
	};

	occ = devm_kzalloc(dev, sizeof(*occ), GFP_KERNEL);
	if (!occ)
		return -ENOMEM;

	occ->sbefifo = dev->parent;
	INIT_LIST_HEAD(&occ->xfrs);
	spin_lock_init(&occ->list_lock);
	mutex_init(&occ->occ_lock);
	INIT_WORK(&occ->work, occ_worker);

	if (dev->of_node) {
		rc = of_property_read_u32(dev->of_node, "reg", &reg);
		if (!rc) {
			/* make sure we don't have a duplicate from dts */
			occ->idx = ida_simple_get(&occ_ida, reg, reg + 1,
						  GFP_KERNEL);
			if (occ->idx < 0)
				occ->idx = ida_simple_get(&occ_ida, 1, INT_MAX,
							  GFP_KERNEL);
		} else {
			occ->idx = ida_simple_get(&occ_ida, 1, INT_MAX,
						  GFP_KERNEL);
		}
	} else {
		occ->idx = ida_simple_get(&occ_ida, 1, INT_MAX, GFP_KERNEL);
	}

	platform_set_drvdata(pdev, occ);

	snprintf(occ->name, sizeof(occ->name), "occ%d", occ->idx);
	occ->mdev.fops = &occ_fops;
	occ->mdev.minor = MISC_DYNAMIC_MINOR;
	occ->mdev.name = occ->name;
	occ->mdev.parent = dev;

	rc = misc_register(&occ->mdev);
	if (rc) {
		dev_err(dev, "failed to register miscdevice: %d\n", rc);
		ida_simple_remove(&occ_ida, occ->idx);
		return rc;
	}

	hwmon_dev_info.id = occ->idx;
	hwmon_dev = platform_device_register_full(&hwmon_dev_info);
	if (!hwmon_dev)
		dev_warn(dev, "failed to create hwmon device\n");

	return 0;
}

static int occ_remove(struct platform_device *pdev)
{
	unsigned long flags;
	struct occ *occ = platform_get_drvdata(pdev);
	struct occ_xfr *xfr;
	struct occ_client *client;

	occ->cancel = true;

	spin_lock_irqsave(&occ->list_lock, flags);
	list_for_each_entry(xfr, &occ->xfrs, link) {
		client = to_client(xfr);
		wake_up_all(&client->wait);
	}
	spin_unlock_irqrestore(&occ->list_lock, flags);

	misc_deregister(&occ->mdev);
	device_for_each_child(&pdev->dev, NULL, occ_unregister_child);

	cancel_work_sync(&occ->work);

	ida_simple_remove(&occ_ida, occ->idx);

	return 0;
}

static const struct of_device_id occ_match[] = {
	{ .compatible = "ibm,p9-occ" },
	{ },
};

static struct platform_driver occ_driver = {
	.driver = {
		.name = "occ",
		.of_match_table	= occ_match,
	},
	.probe	= occ_probe,
	.remove = occ_remove,
};

static int occ_init(void)
{
	occ_wq = create_singlethread_workqueue("occ");
	if (!occ_wq)
		return -ENOMEM;

	return platform_driver_register(&occ_driver);
}

static void occ_exit(void)
{
	destroy_workqueue(occ_wq);

	platform_driver_unregister(&occ_driver);

	ida_destroy(&occ_ida);
}

module_init(occ_init);
module_exit(occ_exit);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("BMC P9 OCC driver");
MODULE_LICENSE("GPL");
