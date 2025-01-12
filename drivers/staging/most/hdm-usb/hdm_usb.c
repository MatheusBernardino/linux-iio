/*
 * hdm_usb.c - Hardware dependent module for USB
 *
 * Copyright (C) 2013-2015 Microchip Technology Germany II GmbH & Co. KG
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This file is licensed under GPLv2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/sysfs.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/uaccess.h>
#include "mostcore.h"

#define USB_MTU			512
#define NO_ISOCHRONOUS_URB	0
#define AV_PACKETS_PER_XACT	2
#define BUF_CHAIN_SIZE		0xFFFF
#define MAX_NUM_ENDPOINTS	30
#define MAX_SUFFIX_LEN		10
#define MAX_STRING_LEN		80
#define MAX_BUF_SIZE		0xFFFF

#define USB_VENDOR_ID_SMSC	0x0424  /* VID: SMSC */
#define USB_DEV_ID_BRDG		0xC001  /* PID: USB Bridge */
#define USB_DEV_ID_OS81118	0xCF18  /* PID: USB OS81118 */
#define USB_DEV_ID_OS81119	0xCF19  /* PID: USB OS81119 */
#define USB_DEV_ID_OS81210	0xCF30  /* PID: USB OS81210 */
/* DRCI Addresses */
#define DRCI_REG_NI_STATE	0x0100
#define DRCI_REG_PACKET_BW	0x0101
#define DRCI_REG_NODE_ADDR	0x0102
#define DRCI_REG_NODE_POS	0x0103
#define DRCI_REG_MEP_FILTER	0x0140
#define DRCI_REG_HASH_TBL0	0x0141
#define DRCI_REG_HASH_TBL1	0x0142
#define DRCI_REG_HASH_TBL2	0x0143
#define DRCI_REG_HASH_TBL3	0x0144
#define DRCI_REG_HW_ADDR_HI	0x0145
#define DRCI_REG_HW_ADDR_MI	0x0146
#define DRCI_REG_HW_ADDR_LO	0x0147
#define DRCI_REG_BASE		0x1100
#define DRCI_COMMAND		0x02
#define DRCI_READ_REQ		0xA0
#define DRCI_WRITE_REQ		0xA1

/**
 * struct most_dci_obj - Direct Communication Interface
 * @kobj:position in sysfs
 * @usb_device: pointer to the usb device
 * @reg_addr: register address for arbitrary DCI access
 */
struct most_dci_obj {
	struct kobject kobj;
	struct usb_device *usb_device;
	u16 reg_addr;
};

#define to_dci_obj(p) container_of(p, struct most_dci_obj, kobj)

struct most_dev;

struct clear_hold_work {
	struct work_struct ws;
	struct most_dev *mdev;
	unsigned int channel;
	int pipe;
};

#define to_clear_hold_work(w) container_of(w, struct clear_hold_work, ws)

/**
 * struct most_dev - holds all usb interface specific stuff
 * @parent: parent object in sysfs
 * @usb_device: pointer to usb device
 * @iface: hardware interface
 * @cap: channel capabilities
 * @conf: channel configuration
 * @dci: direct communication interface of hardware
 * @ep_address: endpoint address table
 * @description: device description
 * @suffix: suffix for channel name
 * @channel_lock: synchronize channel access
 * @padding_active: indicates channel uses padding
 * @is_channel_healthy: health status table of each channel
 * @busy_urbs: list of anchored items
 * @io_mutex: synchronize I/O with disconnect
 * @link_stat_timer: timer for link status reports
 * @poll_work_obj: work for polling link status
 */
struct most_dev {
	struct kobject *parent;
	struct usb_device *usb_device;
	struct most_interface iface;
	struct most_channel_capability *cap;
	struct most_channel_config *conf;
	struct most_dci_obj *dci;
	u8 *ep_address;
	char description[MAX_STRING_LEN];
	char suffix[MAX_NUM_ENDPOINTS][MAX_SUFFIX_LEN];
	spinlock_t channel_lock[MAX_NUM_ENDPOINTS]; /* sync channel access */
	bool padding_active[MAX_NUM_ENDPOINTS];
	bool is_channel_healthy[MAX_NUM_ENDPOINTS];
	struct clear_hold_work clear_work[MAX_NUM_ENDPOINTS];
	struct usb_anchor *busy_urbs;
	struct mutex io_mutex;
	struct timer_list link_stat_timer;
	struct work_struct poll_work_obj;
	void (*on_netinfo)(struct most_interface *, unsigned char,
			   unsigned char *);
};

#define to_mdev(d) container_of(d, struct most_dev, iface)
#define to_mdev_from_work(w) container_of(w, struct most_dev, poll_work_obj)

static void wq_clear_halt(struct work_struct *wq_obj);
static void wq_netinfo(struct work_struct *wq_obj);

/**
 * drci_rd_reg - read a DCI register
 * @dev: usb device
 * @reg: register address
 * @buf: buffer to store data
 *
 * This is reads data from INIC's direct register communication interface
 */
static inline int drci_rd_reg(struct usb_device *dev, u16 reg, u16 *buf)
{
	int retval;
	__le16 *dma_buf = kzalloc(sizeof(*dma_buf), GFP_KERNEL);
	u8 req_type = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE;

	if (!dma_buf)
		return -ENOMEM;

	retval = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
				 DRCI_READ_REQ, req_type,
				 0x0000,
				 reg, dma_buf, sizeof(*dma_buf), 5 * HZ);
	*buf = le16_to_cpu(*dma_buf);
	kfree(dma_buf);

	return retval;
}

/**
 * drci_wr_reg - write a DCI register
 * @dev: usb device
 * @reg: register address
 * @data: data to write
 *
 * This is writes data to INIC's direct register communication interface
 */
static inline int drci_wr_reg(struct usb_device *dev, u16 reg, u16 data)
{
	return usb_control_msg(dev,
			       usb_sndctrlpipe(dev, 0),
			       DRCI_WRITE_REQ,
			       USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			       data,
			       reg,
			       NULL,
			       0,
			       5 * HZ);
}

static inline int start_sync_ep(struct usb_device *usb_dev, u16 ep)
{
	return drci_wr_reg(usb_dev, DRCI_REG_BASE + DRCI_COMMAND + ep * 16, 1);
}

/**
 * get_stream_frame_size - calculate frame size of current configuration
 * @cfg: channel configuration
 */
static unsigned int get_stream_frame_size(struct most_channel_config *cfg)
{
	unsigned int frame_size = 0;
	unsigned int sub_size = cfg->subbuffer_size;

	if (!sub_size) {
		pr_warn("Misconfig: Subbuffer size zero.\n");
		return frame_size;
	}
	switch (cfg->data_type) {
	case MOST_CH_ISOC:
		frame_size = AV_PACKETS_PER_XACT * sub_size;
		break;
	case MOST_CH_SYNC:
		if (cfg->packets_per_xact == 0) {
			pr_warn("Misconfig: Packets per XACT zero\n");
			frame_size = 0;
		} else if (cfg->packets_per_xact == 0xFF) {
			frame_size = (USB_MTU / sub_size) * sub_size;
		} else {
			frame_size = cfg->packets_per_xact * sub_size;
		}
		break;
	default:
		pr_warn("Query frame size of non-streaming channel\n");
		break;
	}
	return frame_size;
}

/**
 * hdm_poison_channel - mark buffers of this channel as invalid
 * @iface: pointer to the interface
 * @channel: channel ID
 *
 * This unlinks all URBs submitted to the HCD,
 * calls the associated completion function of the core and removes
 * them from the list.
 *
 * Returns 0 on success or error code otherwise.
 */
static int hdm_poison_channel(struct most_interface *iface, int channel)
{
	struct most_dev *mdev = to_mdev(iface);
	unsigned long flags;
	spinlock_t *lock; /* temp. lock */

	if (unlikely(!iface)) {
		dev_warn(&mdev->usb_device->dev, "Poison: Bad interface.\n");
		return -EIO;
	}
	if (unlikely(channel < 0 || channel >= iface->num_channels)) {
		dev_warn(&mdev->usb_device->dev, "Channel ID out of range.\n");
		return -ECHRNG;
	}

	lock = mdev->channel_lock + channel;
	spin_lock_irqsave(lock, flags);
	mdev->is_channel_healthy[channel] = false;
	spin_unlock_irqrestore(lock, flags);

	cancel_work_sync(&mdev->clear_work[channel].ws);

	mutex_lock(&mdev->io_mutex);
	usb_kill_anchored_urbs(&mdev->busy_urbs[channel]);
	if (mdev->padding_active[channel])
		mdev->padding_active[channel] = false;

	if (mdev->conf[channel].data_type == MOST_CH_ASYNC) {
		del_timer_sync(&mdev->link_stat_timer);
		cancel_work_sync(&mdev->poll_work_obj);
	}
	mutex_unlock(&mdev->io_mutex);
	return 0;
}

/**
 * hdm_add_padding - add padding bytes
 * @mdev: most device
 * @channel: channel ID
 * @mbo: buffer object
 *
 * This inserts the INIC hardware specific padding bytes into a streaming
 * channel's buffer
 */
static int hdm_add_padding(struct most_dev *mdev, int channel, struct mbo *mbo)
{
	struct most_channel_config *conf = &mdev->conf[channel];
	unsigned int frame_size = get_stream_frame_size(conf);
	unsigned int j, num_frames;

	if (!frame_size)
		return -EIO;
	num_frames = mbo->buffer_length / frame_size;

	if (num_frames < 1) {
		dev_err(&mdev->usb_device->dev,
			"Missed minimal transfer unit.\n");
		return -EIO;
	}

	for (j = num_frames - 1; j > 0; j--)
		memmove(mbo->virt_address + j * USB_MTU,
			mbo->virt_address + j * frame_size,
			frame_size);
	mbo->buffer_length = num_frames * USB_MTU;
	return 0;
}

/**
 * hdm_remove_padding - remove padding bytes
 * @mdev: most device
 * @channel: channel ID
 * @mbo: buffer object
 *
 * This takes the INIC hardware specific padding bytes off a streaming
 * channel's buffer.
 */
static int hdm_remove_padding(struct most_dev *mdev, int channel,
			      struct mbo *mbo)
{
	struct most_channel_config *const conf = &mdev->conf[channel];
	unsigned int frame_size = get_stream_frame_size(conf);
	unsigned int j, num_frames;

	if (!frame_size)
		return -EIO;
	num_frames = mbo->processed_length / USB_MTU;

	for (j = 1; j < num_frames; j++)
		memmove(mbo->virt_address + frame_size * j,
			mbo->virt_address + USB_MTU * j,
			frame_size);

	mbo->processed_length = frame_size * num_frames;
	return 0;
}

/**
 * hdm_write_completion - completion function for submitted Tx URBs
 * @urb: the URB that has been completed
 *
 * This checks the status of the completed URB. In case the URB has been
 * unlinked before, it is immediately freed. On any other error the MBO
 * transfer flag is set. On success it frees allocated resources and calls
 * the completion function.
 *
 * Context: interrupt!
 */
static void hdm_write_completion(struct urb *urb)
{
	struct mbo *mbo = urb->context;
	struct most_dev *mdev = to_mdev(mbo->ifp);
	unsigned int channel = mbo->hdm_channel_id;
	struct device *dev = &mdev->usb_device->dev;
	spinlock_t *lock = mdev->channel_lock + channel;
	unsigned long flags;

	spin_lock_irqsave(lock, flags);

	mbo->processed_length = 0;
	mbo->status = MBO_E_INVAL;
	if (likely(mdev->is_channel_healthy[channel])) {
		switch (urb->status) {
		case 0:
		case -ESHUTDOWN:
			mbo->processed_length = urb->actual_length;
			mbo->status = MBO_SUCCESS;
			break;
		case -EPIPE:
			dev_warn(dev, "Broken OUT pipe detected\n");
			mdev->is_channel_healthy[channel] = false;
			mdev->clear_work[channel].pipe = urb->pipe;
			schedule_work(&mdev->clear_work[channel].ws);
			break;
		case -ENODEV:
		case -EPROTO:
			mbo->status = MBO_E_CLOSE;
			break;
		}
	}

	spin_unlock_irqrestore(lock, flags);

	if (likely(mbo->complete))
		mbo->complete(mbo);
	usb_free_urb(urb);
}

/**
 * hdm_read_completion - completion function for submitted Rx URBs
 * @urb: the URB that has been completed
 *
 * This checks the status of the completed URB. In case the URB has been
 * unlinked before it is immediately freed. On any other error the MBO transfer
 * flag is set. On success it frees allocated resources, removes
 * padding bytes -if necessary- and calls the completion function.
 *
 * Context: interrupt!
 *
 * **************************************************************************
 *                   Error codes returned by in urb->status
 *                   or in iso_frame_desc[n].status (for ISO)
 * *************************************************************************
 *
 * USB device drivers may only test urb status values in completion handlers.
 * This is because otherwise there would be a race between HCDs updating
 * these values on one CPU, and device drivers testing them on another CPU.
 *
 * A transfer's actual_length may be positive even when an error has been
 * reported.  That's because transfers often involve several packets, so that
 * one or more packets could finish before an error stops further endpoint I/O.
 *
 * For isochronous URBs, the urb status value is non-zero only if the URB is
 * unlinked, the device is removed, the host controller is disabled or the total
 * transferred length is less than the requested length and the URB_SHORT_NOT_OK
 * flag is set.  Completion handlers for isochronous URBs should only see
 * urb->status set to zero, -ENOENT, -ECONNRESET, -ESHUTDOWN, or -EREMOTEIO.
 * Individual frame descriptor status fields may report more status codes.
 *
 *
 * 0			Transfer completed successfully
 *
 * -ENOENT		URB was synchronously unlinked by usb_unlink_urb
 *
 * -EINPROGRESS		URB still pending, no results yet
 *			(That is, if drivers see this it's a bug.)
 *
 * -EPROTO (*, **)	a) bitstuff error
 *			b) no response packet received within the
 *			   prescribed bus turn-around time
 *			c) unknown USB error
 *
 * -EILSEQ (*, **)	a) CRC mismatch
 *			b) no response packet received within the
 *			   prescribed bus turn-around time
 *			c) unknown USB error
 *
 *			Note that often the controller hardware does not
 *			distinguish among cases a), b), and c), so a
 *			driver cannot tell whether there was a protocol
 *			error, a failure to respond (often caused by
 *			device disconnect), or some other fault.
 *
 * -ETIME (**)		No response packet received within the prescribed
 *			bus turn-around time.  This error may instead be
 *			reported as -EPROTO or -EILSEQ.
 *
 * -ETIMEDOUT		Synchronous USB message functions use this code
 *			to indicate timeout expired before the transfer
 *			completed, and no other error was reported by HC.
 *
 * -EPIPE (**)		Endpoint stalled.  For non-control endpoints,
 *			reset this status with usb_clear_halt().
 *
 * -ECOMM		During an IN transfer, the host controller
 *			received data from an endpoint faster than it
 *			could be written to system memory
 *
 * -ENOSR		During an OUT transfer, the host controller
 *			could not retrieve data from system memory fast
 *			enough to keep up with the USB data rate
 *
 * -EOVERFLOW (*)	The amount of data returned by the endpoint was
 *			greater than either the max packet size of the
 *			endpoint or the remaining buffer size.  "Babble".
 *
 * -EREMOTEIO		The data read from the endpoint did not fill the
 *			specified buffer, and URB_SHORT_NOT_OK was set in
 *			urb->transfer_flags.
 *
 * -ENODEV		Device was removed.  Often preceded by a burst of
 *			other errors, since the hub driver doesn't detect
 *			device removal events immediately.
 *
 * -EXDEV		ISO transfer only partially completed
 *			(only set in iso_frame_desc[n].status, not urb->status)
 *
 * -EINVAL		ISO madness, if this happens: Log off and go home
 *
 * -ECONNRESET		URB was asynchronously unlinked by usb_unlink_urb
 *
 * -ESHUTDOWN		The device or host controller has been disabled due
 *			to some problem that could not be worked around,
 *			such as a physical disconnect.
 *
 *
 * (*) Error codes like -EPROTO, -EILSEQ and -EOVERFLOW normally indicate
 * hardware problems such as bad devices (including firmware) or cables.
 *
 * (**) This is also one of several codes that different kinds of host
 * controller use to indicate a transfer has failed because of device
 * disconnect.  In the interval before the hub driver starts disconnect
 * processing, devices may receive such fault reports for every request.
 *
 * See <https://www.kernel.org/doc/Documentation/driver-api/usb/error-codes.rst>
 */
static void hdm_read_completion(struct urb *urb)
{
	struct mbo *mbo = urb->context;
	struct most_dev *mdev = to_mdev(mbo->ifp);
	unsigned int channel = mbo->hdm_channel_id;
	struct device *dev = &mdev->usb_device->dev;
	spinlock_t *lock = mdev->channel_lock + channel;
	unsigned long flags;

	spin_lock_irqsave(lock, flags);

	mbo->processed_length = 0;
	mbo->status = MBO_E_INVAL;
	if (likely(mdev->is_channel_healthy[channel])) {
		switch (urb->status) {
		case 0:
		case -ESHUTDOWN:
			mbo->processed_length = urb->actual_length;
			mbo->status = MBO_SUCCESS;
			if (mdev->padding_active[channel] &&
			    hdm_remove_padding(mdev, channel, mbo)) {
				mbo->processed_length = 0;
				mbo->status = MBO_E_INVAL;
			}
			break;
		case -EPIPE:
			dev_warn(dev, "Broken IN pipe detected\n");
			mdev->is_channel_healthy[channel] = false;
			mdev->clear_work[channel].pipe = urb->pipe;
			schedule_work(&mdev->clear_work[channel].ws);
			break;
		case -ENODEV:
		case -EPROTO:
			mbo->status = MBO_E_CLOSE;
			break;
		case -EOVERFLOW:
			dev_warn(dev, "Babble on IN pipe detected\n");
			break;
		}
	}

	spin_unlock_irqrestore(lock, flags);

	if (likely(mbo->complete))
		mbo->complete(mbo);
	usb_free_urb(urb);
}

/**
 * hdm_enqueue - receive a buffer to be used for data transfer
 * @iface: interface to enqueue to
 * @channel: ID of the channel
 * @mbo: pointer to the buffer object
 *
 * This allocates a new URB and fills it according to the channel
 * that is being used for transmission of data. Before the URB is
 * submitted it is stored in the private anchor list.
 *
 * Returns 0 on success. On any error the URB is freed and a error code
 * is returned.
 *
 * Context: Could in _some_ cases be interrupt!
 */
static int hdm_enqueue(struct most_interface *iface, int channel,
		       struct mbo *mbo)
{
	struct most_dev *mdev;
	struct most_channel_config *conf;
	struct device *dev;
	int retval = 0;
	struct urb *urb;
	unsigned long length;
	void *virt_address;

	if (unlikely(!iface || !mbo))
		return -EIO;
	if (unlikely(iface->num_channels <= channel || channel < 0))
		return -ECHRNG;

	mdev = to_mdev(iface);
	conf = &mdev->conf[channel];
	dev = &mdev->usb_device->dev;

	if (!mdev->usb_device)
		return -ENODEV;

	urb = usb_alloc_urb(NO_ISOCHRONOUS_URB, GFP_ATOMIC);
	if (!urb)
		return -ENOMEM;

	if ((conf->direction & MOST_CH_TX) && mdev->padding_active[channel] &&
	    hdm_add_padding(mdev, channel, mbo)) {
		retval = -EIO;
		goto _error;
	}

	urb->transfer_dma = mbo->bus_address;
	virt_address = mbo->virt_address;
	length = mbo->buffer_length;

	if (conf->direction & MOST_CH_TX) {
		usb_fill_bulk_urb(urb, mdev->usb_device,
				  usb_sndbulkpipe(mdev->usb_device,
						  mdev->ep_address[channel]),
				  virt_address,
				  length,
				  hdm_write_completion,
				  mbo);
		if (conf->data_type != MOST_CH_ISOC)
			urb->transfer_flags |= URB_ZERO_PACKET;
	} else {
		usb_fill_bulk_urb(urb, mdev->usb_device,
				  usb_rcvbulkpipe(mdev->usb_device,
						  mdev->ep_address[channel]),
				  virt_address,
				  length + conf->extra_len,
				  hdm_read_completion,
				  mbo);
	}
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_anchor_urb(urb, &mdev->busy_urbs[channel]);

	retval = usb_submit_urb(urb, GFP_KERNEL);
	if (retval) {
		dev_err(dev, "URB submit failed with error %d.\n", retval);
		goto _error_1;
	}
	return 0;

_error_1:
	usb_unanchor_urb(urb);
_error:
	usb_free_urb(urb);
	return retval;
}

/**
 * hdm_configure_channel - receive channel configuration from core
 * @iface: interface
 * @channel: channel ID
 * @conf: structure that holds the configuration information
 *
 * The attached network interface controller (NIC) supports a padding mode
 * to avoid short packets on USB, hence increasing the performance due to a
 * lower interrupt load. This mode is default for synchronous data and can
 * be switched on for isochronous data. In case padding is active the
 * driver needs to know the frame size of the payload in order to calculate
 * the number of bytes it needs to pad when transmitting or to cut off when
 * receiving data.
 *
 */
static int hdm_configure_channel(struct most_interface *iface, int channel,
				 struct most_channel_config *conf)
{
	unsigned int num_frames;
	unsigned int frame_size;
	struct most_dev *mdev = to_mdev(iface);
	struct device *dev = &mdev->usb_device->dev;

	mdev->is_channel_healthy[channel] = true;
	mdev->clear_work[channel].channel = channel;
	mdev->clear_work[channel].mdev = mdev;
	INIT_WORK(&mdev->clear_work[channel].ws, wq_clear_halt);

	if (unlikely(!iface || !conf)) {
		dev_err(dev, "Bad interface or config pointer.\n");
		return -EINVAL;
	}
	if (unlikely(channel < 0 || channel >= iface->num_channels)) {
		dev_err(dev, "Channel ID out of range.\n");
		return -EINVAL;
	}
	if (!conf->num_buffers || !conf->buffer_size) {
		dev_err(dev, "Misconfig: buffer size or #buffers zero.\n");
		return -EINVAL;
	}

	if (conf->data_type != MOST_CH_SYNC &&
	    !(conf->data_type == MOST_CH_ISOC &&
	      conf->packets_per_xact != 0xFF)) {
		mdev->padding_active[channel] = false;
		/*
		 * Since the NIC's padding mode is not going to be
		 * used, we can skip the frame size calculations and
		 * move directly on to exit.
		 */
		goto exit;
	}

	mdev->padding_active[channel] = true;

	frame_size = get_stream_frame_size(conf);
	if (frame_size == 0 || frame_size > USB_MTU) {
		dev_warn(dev, "Misconfig: frame size wrong\n");
		return -EINVAL;
	}

	num_frames = conf->buffer_size / frame_size;

	if (conf->buffer_size % frame_size) {
		u16 old_size = conf->buffer_size;

		conf->buffer_size = num_frames * frame_size;
		dev_warn(dev, "%s: fixed buffer size (%d -> %d)\n",
			 mdev->suffix[channel], old_size, conf->buffer_size);
	}

	/* calculate extra length to comply w/ HW padding */
	conf->extra_len = num_frames * (USB_MTU - frame_size);

exit:
	mdev->conf[channel] = *conf;
	if (conf->data_type == MOST_CH_ASYNC) {
		u16 ep = mdev->ep_address[channel];

		if (start_sync_ep(mdev->usb_device, ep) < 0)
			dev_warn(dev, "sync for ep%02x failed", ep);
	}
	return 0;
}

/**
 * hdm_request_netinfo - request network information
 * @iface: pointer to interface
 * @channel: channel ID
 *
 * This is used as trigger to set up the link status timer that
 * polls for the NI state of the INIC every 2 seconds.
 *
 */
static void hdm_request_netinfo(struct most_interface *iface, int channel,
				void (*on_netinfo)(struct most_interface *,
						   unsigned char,
						   unsigned char *))
{
	struct most_dev *mdev;

	BUG_ON(!iface);
	mdev = to_mdev(iface);
	mdev->on_netinfo = on_netinfo;
	if (!on_netinfo)
		return;

	mdev->link_stat_timer.expires = jiffies + HZ;
	mod_timer(&mdev->link_stat_timer, mdev->link_stat_timer.expires);
}

/**
 * link_stat_timer_handler - schedule work obtaining mac address and link status
 * @data: pointer to USB device instance
 *
 * The handler runs in interrupt context. That's why we need to defer the
 * tasks to a work queue.
 */
static void link_stat_timer_handler(unsigned long data)
{
	struct most_dev *mdev = (struct most_dev *)data;

	schedule_work(&mdev->poll_work_obj);
	mdev->link_stat_timer.expires = jiffies + (2 * HZ);
	add_timer(&mdev->link_stat_timer);
}

/**
 * wq_netinfo - work queue function to deliver latest networking information
 * @wq_obj: object that holds data for our deferred work to do
 *
 * This retrieves the network interface status of the USB INIC
 */
static void wq_netinfo(struct work_struct *wq_obj)
{
	struct most_dev *mdev = to_mdev_from_work(wq_obj);
	struct usb_device *usb_device = mdev->usb_device;
	struct device *dev = &usb_device->dev;
	u16 hi, mi, lo, link;
	u8 hw_addr[6];

	if (drci_rd_reg(usb_device, DRCI_REG_HW_ADDR_HI, &hi) < 0) {
		dev_err(dev, "Vendor request 'hw_addr_hi' failed\n");
		return;
	}

	if (drci_rd_reg(usb_device, DRCI_REG_HW_ADDR_MI, &mi) < 0) {
		dev_err(dev, "Vendor request 'hw_addr_mid' failed\n");
		return;
	}

	if (drci_rd_reg(usb_device, DRCI_REG_HW_ADDR_LO, &lo) < 0) {
		dev_err(dev, "Vendor request 'hw_addr_low' failed\n");
		return;
	}

	if (drci_rd_reg(usb_device, DRCI_REG_NI_STATE, &link) < 0) {
		dev_err(dev, "Vendor request 'link status' failed\n");
		return;
	}

	hw_addr[0] = hi >> 8;
	hw_addr[1] = hi;
	hw_addr[2] = mi >> 8;
	hw_addr[3] = mi;
	hw_addr[4] = lo >> 8;
	hw_addr[5] = lo;

	if (mdev->on_netinfo)
		mdev->on_netinfo(&mdev->iface, link, hw_addr);
}

/**
 * wq_clear_halt - work queue function
 * @wq_obj: work_struct object to execute
 *
 * This sends a clear_halt to the given USB pipe.
 */
static void wq_clear_halt(struct work_struct *wq_obj)
{
	struct clear_hold_work *clear_work = to_clear_hold_work(wq_obj);
	struct most_dev *mdev = clear_work->mdev;
	unsigned int channel = clear_work->channel;
	int pipe = clear_work->pipe;

	mutex_lock(&mdev->io_mutex);
	most_stop_enqueue(&mdev->iface, channel);
	usb_kill_anchored_urbs(&mdev->busy_urbs[channel]);
	if (usb_clear_halt(mdev->usb_device, pipe))
		dev_warn(&mdev->usb_device->dev, "Failed to reset endpoint.\n");

	mdev->is_channel_healthy[channel] = true;
	most_resume_enqueue(&mdev->iface, channel);
	mutex_unlock(&mdev->io_mutex);
}

/**
 * hdm_usb_fops - file operation table for USB driver
 */
static const struct file_operations hdm_usb_fops = {
	.owner = THIS_MODULE,
};

/**
 * usb_device_id - ID table for HCD device probing
 */
static const struct usb_device_id usbid[] = {
	{ USB_DEVICE(USB_VENDOR_ID_SMSC, USB_DEV_ID_BRDG), },
	{ USB_DEVICE(USB_VENDOR_ID_SMSC, USB_DEV_ID_OS81118), },
	{ USB_DEVICE(USB_VENDOR_ID_SMSC, USB_DEV_ID_OS81119), },
	{ USB_DEVICE(USB_VENDOR_ID_SMSC, USB_DEV_ID_OS81210), },
	{ } /* Terminating entry */
};

#define MOST_DCI_RO_ATTR(_name) \
	struct most_dci_attribute most_dci_attr_##_name = \
		__ATTR(_name, 0444, show_value, NULL)

#define MOST_DCI_ATTR(_name) \
	struct most_dci_attribute most_dci_attr_##_name = \
		__ATTR(_name, 0644, show_value, store_value)

#define MOST_DCI_WO_ATTR(_name) \
	struct most_dci_attribute most_dci_attr_##_name = \
		__ATTR(_name, 0200, NULL, store_value)

/**
 * struct most_dci_attribute - to access the attributes of a dci object
 * @attr: attributes of a dci object
 * @show: pointer to the show function
 * @store: pointer to the store function
 */
struct most_dci_attribute {
	struct attribute attr;
	ssize_t (*show)(struct most_dci_obj *d,
			struct most_dci_attribute *attr,
			char *buf);
	ssize_t (*store)(struct most_dci_obj *d,
			 struct most_dci_attribute *attr,
			 const char *buf,
			 size_t count);
};

#define to_dci_attr(a) container_of(a, struct most_dci_attribute, attr)

/**
 * dci_attr_show - show function for dci object
 * @kobj: pointer to kobject
 * @attr: pointer to attribute struct
 * @buf: buffer
 */
static ssize_t dci_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
	struct most_dci_attribute *dci_attr = to_dci_attr(attr);
	struct most_dci_obj *dci_obj = to_dci_obj(kobj);

	if (!dci_attr->show)
		return -EIO;

	return dci_attr->show(dci_obj, dci_attr, buf);
}

/**
 * dci_attr_store - store function for dci object
 * @kobj: pointer to kobject
 * @attr: pointer to attribute struct
 * @buf: buffer
 * @len: length of buffer
 */
static ssize_t dci_attr_store(struct kobject *kobj,
			      struct attribute *attr,
			      const char *buf,
			      size_t len)
{
	struct most_dci_attribute *dci_attr = to_dci_attr(attr);
	struct most_dci_obj *dci_obj = to_dci_obj(kobj);

	if (!dci_attr->store)
		return -EIO;

	return dci_attr->store(dci_obj, dci_attr, buf, len);
}

static const struct sysfs_ops most_dci_sysfs_ops = {
	.show = dci_attr_show,
	.store = dci_attr_store,
};

/**
 * most_dci_release - release function for dci object
 * @kobj: pointer to kobject
 *
 * This frees the memory allocated for the dci object
 */
static void most_dci_release(struct kobject *kobj)
{
	struct most_dci_obj *dci_obj = to_dci_obj(kobj);

	kfree(dci_obj);
}

struct regs {
	const char *name;
	u16 reg;
};

static const struct regs ro_regs[] = {
	{ "ni_state", DRCI_REG_NI_STATE },
	{ "packet_bandwidth", DRCI_REG_PACKET_BW },
	{ "node_address", DRCI_REG_NODE_ADDR },
	{ "node_position", DRCI_REG_NODE_POS },
};

static const struct regs rw_regs[] = {
	{ "mep_filter", DRCI_REG_MEP_FILTER },
	{ "mep_hash0", DRCI_REG_HASH_TBL0 },
	{ "mep_hash1", DRCI_REG_HASH_TBL1 },
	{ "mep_hash2", DRCI_REG_HASH_TBL2 },
	{ "mep_hash3", DRCI_REG_HASH_TBL3 },
	{ "mep_eui48_hi", DRCI_REG_HW_ADDR_HI },
	{ "mep_eui48_mi", DRCI_REG_HW_ADDR_MI },
	{ "mep_eui48_lo", DRCI_REG_HW_ADDR_LO },
};

static int get_stat_reg_addr(const struct regs *regs, int size,
			     const char *name, u16 *reg_addr)
{
	int i;

	for (i = 0; i < size; i++) {
		if (!strcmp(name, regs[i].name)) {
			*reg_addr = regs[i].reg;
			return 0;
		}
	}
	return -EFAULT;
}

#define get_static_reg_addr(regs, name, reg_addr) \
	get_stat_reg_addr(regs, ARRAY_SIZE(regs), name, reg_addr)

static ssize_t show_value(struct most_dci_obj *dci_obj,
			  struct most_dci_attribute *attr, char *buf)
{
	const char *name = attr->attr.name;
	u16 val;
	u16 reg_addr;
	int err;

	if (!strcmp(name, "arb_address"))
		return snprintf(buf, PAGE_SIZE, "%04x\n", dci_obj->reg_addr);

	if (!strcmp(name, "arb_value"))
		reg_addr = dci_obj->reg_addr;
	else if (get_static_reg_addr(ro_regs, name, &reg_addr) &&
		 get_static_reg_addr(rw_regs, name, &reg_addr))
		return -EFAULT;

	err = drci_rd_reg(dci_obj->usb_device, reg_addr, &val);
	if (err < 0)
		return err;

	return snprintf(buf, PAGE_SIZE, "%04x\n", val);
}

static ssize_t store_value(struct most_dci_obj *dci_obj,
			   struct most_dci_attribute *attr,
			   const char *buf, size_t count)
{
	u16 val;
	u16 reg_addr;
	const char *name = attr->attr.name;
	struct usb_device *usb_dev = dci_obj->usb_device;
	int err = kstrtou16(buf, 16, &val);

	if (err)
		return err;

	if (!strcmp(name, "arb_address")) {
		dci_obj->reg_addr = val;
		return count;
	}

	if (!strcmp(name, "arb_value"))
		err = drci_wr_reg(usb_dev, dci_obj->reg_addr, val);
	else if (!strcmp(name, "sync_ep"))
		err = start_sync_ep(usb_dev, val);
	else if (!get_static_reg_addr(rw_regs, name, &reg_addr))
		err = drci_wr_reg(usb_dev, reg_addr, val);
	else
		return -EFAULT;

	if (err < 0)
		return err;

	return count;
}

static MOST_DCI_RO_ATTR(ni_state);
static MOST_DCI_RO_ATTR(packet_bandwidth);
static MOST_DCI_RO_ATTR(node_address);
static MOST_DCI_RO_ATTR(node_position);
static MOST_DCI_WO_ATTR(sync_ep);
static MOST_DCI_ATTR(mep_filter);
static MOST_DCI_ATTR(mep_hash0);
static MOST_DCI_ATTR(mep_hash1);
static MOST_DCI_ATTR(mep_hash2);
static MOST_DCI_ATTR(mep_hash3);
static MOST_DCI_ATTR(mep_eui48_hi);
static MOST_DCI_ATTR(mep_eui48_mi);
static MOST_DCI_ATTR(mep_eui48_lo);
static MOST_DCI_ATTR(arb_address);
static MOST_DCI_ATTR(arb_value);

/**
 * most_dci_def_attrs - array of default attribute files of the dci object
 */
static struct attribute *most_dci_def_attrs[] = {
	&most_dci_attr_ni_state.attr,
	&most_dci_attr_packet_bandwidth.attr,
	&most_dci_attr_node_address.attr,
	&most_dci_attr_node_position.attr,
	&most_dci_attr_sync_ep.attr,
	&most_dci_attr_mep_filter.attr,
	&most_dci_attr_mep_hash0.attr,
	&most_dci_attr_mep_hash1.attr,
	&most_dci_attr_mep_hash2.attr,
	&most_dci_attr_mep_hash3.attr,
	&most_dci_attr_mep_eui48_hi.attr,
	&most_dci_attr_mep_eui48_mi.attr,
	&most_dci_attr_mep_eui48_lo.attr,
	&most_dci_attr_arb_address.attr,
	&most_dci_attr_arb_value.attr,
	NULL,
};

/**
 * DCI ktype
 */
static struct kobj_type most_dci_ktype = {
	.sysfs_ops = &most_dci_sysfs_ops,
	.release = most_dci_release,
	.default_attrs = most_dci_def_attrs,
};

/**
 * create_most_dci_obj - allocates a dci object
 * @parent: parent kobject
 *
 * This creates a dci object and registers it with sysfs.
 * Returns a pointer to the object or NULL when something went wrong.
 */
static struct
most_dci_obj *create_most_dci_obj(struct kobject *parent)
{
	struct most_dci_obj *most_dci = kzalloc(sizeof(*most_dci), GFP_KERNEL);
	int retval;

	if (!most_dci)
		return NULL;

	retval = kobject_init_and_add(&most_dci->kobj, &most_dci_ktype, parent,
				      "dci");
	if (retval) {
		kobject_put(&most_dci->kobj);
		return NULL;
	}
	return most_dci;
}

/**
 * destroy_most_dci_obj - DCI object release function
 * @p: pointer to dci object
 */
static void destroy_most_dci_obj(struct most_dci_obj *p)
{
	kobject_put(&p->kobj);
}

/**
 * hdm_probe - probe function of USB device driver
 * @interface: Interface of the attached USB device
 * @id: Pointer to the USB ID table.
 *
 * This allocates and initializes the device instance, adds the new
 * entry to the internal list, scans the USB descriptors and registers
 * the interface with the core.
 * Additionally, the DCI objects are created and the hardware is sync'd.
 *
 * Return 0 on success. In case of an error a negative number is returned.
 */
static int
hdm_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_host_interface *usb_iface_desc = interface->cur_altsetting;
	struct usb_device *usb_dev = interface_to_usbdev(interface);
	struct device *dev = &usb_dev->dev;
	struct most_dev *mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	unsigned int i;
	unsigned int num_endpoints;
	struct most_channel_capability *tmp_cap;
	struct usb_endpoint_descriptor *ep_desc;
	int ret = 0;

	if (!mdev)
		goto exit_ENOMEM;

	usb_set_intfdata(interface, mdev);
	num_endpoints = usb_iface_desc->desc.bNumEndpoints;
	mutex_init(&mdev->io_mutex);
	INIT_WORK(&mdev->poll_work_obj, wq_netinfo);
	setup_timer(&mdev->link_stat_timer, link_stat_timer_handler,
		    (unsigned long)mdev);

	mdev->usb_device = usb_dev;
	mdev->link_stat_timer.expires = jiffies + (2 * HZ);

	mdev->iface.mod = hdm_usb_fops.owner;
	mdev->iface.interface = ITYPE_USB;
	mdev->iface.configure = hdm_configure_channel;
	mdev->iface.request_netinfo = hdm_request_netinfo;
	mdev->iface.enqueue = hdm_enqueue;
	mdev->iface.poison_channel = hdm_poison_channel;
	mdev->iface.description = mdev->description;
	mdev->iface.num_channels = num_endpoints;

	snprintf(mdev->description, sizeof(mdev->description),
		 "usb_device %d-%s:%d.%d",
		 usb_dev->bus->busnum,
		 usb_dev->devpath,
		 usb_dev->config->desc.bConfigurationValue,
		 usb_iface_desc->desc.bInterfaceNumber);

	mdev->conf = kcalloc(num_endpoints, sizeof(*mdev->conf), GFP_KERNEL);
	if (!mdev->conf)
		goto exit_free;

	mdev->cap = kcalloc(num_endpoints, sizeof(*mdev->cap), GFP_KERNEL);
	if (!mdev->cap)
		goto exit_free1;

	mdev->iface.channel_vector = mdev->cap;
	mdev->iface.priv = NULL;

	mdev->ep_address =
		kcalloc(num_endpoints, sizeof(*mdev->ep_address), GFP_KERNEL);
	if (!mdev->ep_address)
		goto exit_free2;

	mdev->busy_urbs =
		kcalloc(num_endpoints, sizeof(*mdev->busy_urbs), GFP_KERNEL);
	if (!mdev->busy_urbs)
		goto exit_free3;

	tmp_cap = mdev->cap;
	for (i = 0; i < num_endpoints; i++) {
		ep_desc = &usb_iface_desc->endpoint[i].desc;
		mdev->ep_address[i] = ep_desc->bEndpointAddress;
		mdev->padding_active[i] = false;
		mdev->is_channel_healthy[i] = true;

		snprintf(&mdev->suffix[i][0], MAX_SUFFIX_LEN, "ep%02x",
			 mdev->ep_address[i]);

		tmp_cap->name_suffix = &mdev->suffix[i][0];
		tmp_cap->buffer_size_packet = MAX_BUF_SIZE;
		tmp_cap->buffer_size_streaming = MAX_BUF_SIZE;
		tmp_cap->num_buffers_packet = BUF_CHAIN_SIZE;
		tmp_cap->num_buffers_streaming = BUF_CHAIN_SIZE;
		tmp_cap->data_type = MOST_CH_CONTROL | MOST_CH_ASYNC |
				     MOST_CH_ISOC | MOST_CH_SYNC;
		if (usb_endpoint_dir_in(ep_desc))
			tmp_cap->direction = MOST_CH_RX;
		else
			tmp_cap->direction = MOST_CH_TX;
		tmp_cap++;
		init_usb_anchor(&mdev->busy_urbs[i]);
		spin_lock_init(&mdev->channel_lock[i]);
	}
	dev_notice(dev, "claimed gadget: Vendor=%4.4x ProdID=%4.4x Bus=%02x Device=%02x\n",
		   le16_to_cpu(usb_dev->descriptor.idVendor),
		   le16_to_cpu(usb_dev->descriptor.idProduct),
		   usb_dev->bus->busnum,
		   usb_dev->devnum);

	dev_notice(dev, "device path: /sys/bus/usb/devices/%d-%s:%d.%d\n",
		   usb_dev->bus->busnum,
		   usb_dev->devpath,
		   usb_dev->config->desc.bConfigurationValue,
		   usb_iface_desc->desc.bInterfaceNumber);

	mdev->parent = most_register_interface(&mdev->iface);
	if (IS_ERR(mdev->parent)) {
		ret = PTR_ERR(mdev->parent);
		goto exit_free4;
	}

	mutex_lock(&mdev->io_mutex);
	if (le16_to_cpu(usb_dev->descriptor.idProduct) == USB_DEV_ID_OS81118 ||
	    le16_to_cpu(usb_dev->descriptor.idProduct) == USB_DEV_ID_OS81119 ||
	    le16_to_cpu(usb_dev->descriptor.idProduct) == USB_DEV_ID_OS81210) {
		/* this increments the reference count of the instance
		 * object of the core
		 */
		mdev->dci = create_most_dci_obj(mdev->parent);
		if (!mdev->dci) {
			mutex_unlock(&mdev->io_mutex);
			most_deregister_interface(&mdev->iface);
			ret = -ENOMEM;
			goto exit_free4;
		}

		kobject_uevent(&mdev->dci->kobj, KOBJ_ADD);
		mdev->dci->usb_device = mdev->usb_device;
	}
	mutex_unlock(&mdev->io_mutex);
	return 0;

exit_free4:
	kfree(mdev->busy_urbs);
exit_free3:
	kfree(mdev->ep_address);
exit_free2:
	kfree(mdev->cap);
exit_free1:
	kfree(mdev->conf);
exit_free:
	kfree(mdev);
exit_ENOMEM:
	if (ret == 0 || ret == -ENOMEM) {
		ret = -ENOMEM;
		dev_err(dev, "out of memory\n");
	}
	return ret;
}

/**
 * hdm_disconnect - disconnect function of USB device driver
 * @interface: Interface of the attached USB device
 *
 * This deregisters the interface with the core, removes the kernel timer
 * and frees resources.
 *
 * Context: hub kernel thread
 */
static void hdm_disconnect(struct usb_interface *interface)
{
	struct most_dev *mdev = usb_get_intfdata(interface);

	mutex_lock(&mdev->io_mutex);
	usb_set_intfdata(interface, NULL);
	mdev->usb_device = NULL;
	mutex_unlock(&mdev->io_mutex);

	del_timer_sync(&mdev->link_stat_timer);
	cancel_work_sync(&mdev->poll_work_obj);

	destroy_most_dci_obj(mdev->dci);
	most_deregister_interface(&mdev->iface);

	kfree(mdev->busy_urbs);
	kfree(mdev->cap);
	kfree(mdev->conf);
	kfree(mdev->ep_address);
	kfree(mdev);
}

static struct usb_driver hdm_usb = {
	.name = "hdm_usb",
	.id_table = usbid,
	.probe = hdm_probe,
	.disconnect = hdm_disconnect,
};

static int __init hdm_usb_init(void)
{
	pr_info("hdm_usb_init()\n");
	if (usb_register(&hdm_usb)) {
		pr_err("could not register hdm_usb driver\n");
		return -EIO;
	}

	return 0;
}

static void __exit hdm_usb_exit(void)
{
	pr_info("hdm_usb_exit()\n");
	usb_deregister(&hdm_usb);
}

module_init(hdm_usb_init);
module_exit(hdm_usb_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Christian Gromm <christian.gromm@microchip.com>");
MODULE_DESCRIPTION("HDM_4_USB");
