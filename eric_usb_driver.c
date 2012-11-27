/*
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>


/* Define these values to match your devices */
#define USB_SKEL_VENDOR_ID	0x1234
#define USB_SKEL_PRODUCT_ID	0x5678

//USB_DEVICE是一個MACRO，定義在usb.h中，幫助建立一個 usb_device_id
/* table of devices that work with this driver */
static const struct usb_device_id skel_table[] = {
	{ USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

//告訴用戶空間的熱插拔和模塊裝載，vid和pid對應什麼硬件設備。以便執行自動掛載。
//第一個參數是設備的類型，如果是USB設備，那自然是usb（如果是PCI設備，那將是pci
MODULE_DEVICE_TABLE(usb, skel_table);


/* Get a minor range for your devices from the usb maintainer */
#define USB_SKEL_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8
/* arbitrarily chosen */

/* Structure to hold all of our device specific stuff */
struct usb_skel {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	int			errors;			/* the last request tanked */
	int			open_count;		/* count the number of openers */
	bool			ongoing_read;		/* a read is going on */
	bool			processed_urb;		/* indicates we haven't processed the urb */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	struct completion	bulk_in_completion;	/* to wait for an ongoing read */
};
#define to_skel_dev(d) container_of(d, struct usb_skel, kref)

static struct usb_driver skel_driver;
static void skel_draw_down(struct usb_skel *dev);


static void showEndPoint(const struct usb_endpoint_descriptor *endpoint)
{
	printk(KERN_ERR "ep->bLength=%x\n", endpoint->bLength);
	printk(KERN_ERR "ep->bDescriptorType=%x\n", endpoint->bDescriptorType);
	printk(KERN_ERR "ep->bEndpointAddress=%x\n", endpoint->bEndpointAddress);
	printk(KERN_ERR "ep->bmAttributes=%x\n", endpoint->bmAttributes);
	printk(KERN_ERR "ep->wMaxPacketSize=%x\n", endpoint->wMaxPacketSize);
	printk(KERN_ERR "ep->bInterval=%x\n", endpoint->bInterval);
}

static void skel_delete(struct kref *kref)
{
	//skel_delete主要作用就是?"1"
	/*
	1.獲得設備
	2.減少設備的引用次數
	3.釋放分配的數據空間
	4.釋放分配的驅動空間
	*/
	printk(KERN_ERR "==eric_delete==\n");

	struct usb_skel *dev = to_skel_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	//釋放批量輸入端口緩衝
	kfree(dev->bulk_in_buffer);
	//釋放設備
	kfree(dev);
}


static int skel_open(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	printk(KERN_INFO "==eric_open==\n");
	subminor = iminor(inode);

	printk(KERN_ERR "subminor=%d\n", subminor);

	// 藉由subminor號，取出對應的interface
	interface = usb_find_interface(&skel_driver, subminor);
	if (!interface) {
		err("%s - error, can't find device for minor %d",
		     __func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	//取出interface所綁定的usb_skel
	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	/* increment our usage count for the device */
	// 取出k-reference?
	kref_get(&dev->kref);

	printk(KERN_ERR "refcount=%d\n", dev->kref.refcount);

	/* lock the device to allow correctly handling errors
	 * in resumption */
	mutex_lock(&dev->io_mutex);

	if (!dev->open_count++) {
		retval = usb_autopm_get_interface(interface);
			if (retval) {
				dev->open_count--;
				mutex_unlock(&dev->io_mutex);
				kref_put(&dev->kref, skel_delete);
				goto exit;
			}
	} /* else { //uncomment this block if you want exclusive open
		retval = -EBUSY;
		dev->open_count--;
		mutex_unlock(&dev->io_mutex);
		kref_put(&dev->kref, skel_delete);
		goto exit;
	} */
	/* prevent the device from being autosuspended */

	/* save our object in the file's private structure */
	file->private_data = dev;
	mutex_unlock(&dev->io_mutex);

exit:
	return retval;
}

static int skel_release(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	mutex_lock(&dev->io_mutex);
	if (!--dev->open_count && dev->interface)
		usb_autopm_put_interface(dev->interface);
	mutex_unlock(&dev->io_mutex);

	/* decrement the count on our device */
	kref_put(&dev->kref, skel_delete);
	return 0;
}

static int skel_flush(struct file *file, fl_owner_t id)
{
	struct usb_skel *dev;
	int res;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* wait for io to stop */
	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	/* read out errors, leave subsequent opens a clean slate */
	spin_lock_irq(&dev->err_lock);
	res = dev->errors ? (dev->errors == -EPIPE ? -EPIPE : -EIO) : 0;
	dev->errors = 0;
	spin_unlock_irq(&dev->err_lock);

	mutex_unlock(&dev->io_mutex);

	return res;
}

static void skel_read_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;

	dev = urb->context;

	spin_lock(&dev->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			err("%s - nonzero write bulk status received: %d",
			    __func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock(&dev->err_lock);

	complete(&dev->bulk_in_completion);
}

static int skel_do_read_io(struct usb_skel *dev, size_t count)
{
	int rv;

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			skel_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		err("%s - failed submitting read urb, error %d",
			__func__, rv);
		dev->bulk_in_filled = 0;
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

static ssize_t skel_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct usb_skel *dev;
	int rv;
	bool ongoing_io;

	printk(KERN_INFO "==eric_Read==\n");
	//取出從open那邊 attach 上來的 usb_skel
	dev = file->private_data;

	//檢查 urb 與 count 是否有配置，urb就是在probe那邊配置的一塊記憶體
	/* if we cannot read at all, return EOF */

	printk(KERN_INFO "dev->bulk_in_urb=%d, count=%d\n", dev->bulk_in_urb, count);

	if (!dev->bulk_in_urb || !count)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	//檢查interface是否有值
	if (!dev->interface) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	printk(KERN_ERR "read_start\n");

	//這邊作一個goto tag, 目的就是要retry
	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read; //這個是一個bool
	spin_unlock_irq(&dev->err_lock);

	printk(KERN_ERR "ongoing_io=%d\n", ongoing_io);
	if (ongoing_io) {

		printk(KERN_ERR "file->f_flags=%d\n", file->f_flags);

		/* nonblocking IO shall not wait */
		if (file->f_flags & O_NONBLOCK) {
			rv = -EAGAIN;
			goto exit;
		}
		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		
		rv = wait_for_completion_interruptible(&dev->bulk_in_completion);

		printk(KERN_ERR "rv=%d\n", rv);
		if (rv < 0)
			goto exit;
		/*
		 * by waiting we also semiprocessed the urb
		 * we must finish now
		 */
		dev->bulk_in_copied = 0;
		dev->processed_urb = 1;
	}

	printk(KERN_ERR "dev->processed_urb=%d\n", dev->processed_urb);
	//等於0時，說明這個URB還沒被處理過，即第一次讀取這個URB
	if (!dev->processed_urb) {
		/*
		 * the URB hasn't been processed
		 * do it now
		 */
		//completion 是任務使用的一個輕量級機制: 允許一個線程告訴另一個線程工作已經完成
		// 詳細請看 http://blog.roodo.com/_jacob_/archives/2884879.html

		printk(KERN_ERR "wfc_start=%d\n", &dev->bulk_in_completion.done);
		wait_for_completion(&dev->bulk_in_completion);
		printk(KERN_ERR "wfc_end=%d\n", &dev->bulk_in_completion.done);

		dev->bulk_in_copied = 0;
		dev->processed_urb = 1;
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* no data to deliver */
		dev->bulk_in_filled = 0;
		/* report it */
		goto exit;
	}

	/*
	 * if the buffer is filled we may satisfy the read
	 * else we need to start IO
	 */
	printk(KERN_ERR "dev->bulk_in_filled=%d\n", dev->bulk_in_filled);
	if (dev->bulk_in_filled) {
		/* we had read data */
		size_t available = dev->bulk_in_filled - dev->bulk_in_copied;
		size_t chunk = min(available, count);

		printk(KERN_ERR "dev->bulk_in_copied=%d, dev->bulk_in_filled=%d\n", dev->bulk_in_copied, dev->bulk_in_filled);
		printk(KERN_ERR "available=%d, chunk=%d\n", available, chunk);
		
		if (!available) {
			/*
			 * all data has been used
			 * actual IO needs to be done
			 */
			rv = skel_do_read_io(dev, count);
			if (rv < 0)
				goto exit;
			else
				goto retry;
		}
		/*
		 * data is available
		 * chunk tells us how much shall be copied
		 */

		if (copy_to_user(buffer,
				 dev->bulk_in_buffer + dev->bulk_in_copied,
				 chunk))
			rv = -EFAULT;
		else
			rv = chunk;

		dev->bulk_in_copied += chunk;

		/*
		 * if we are asked for more than we have,
		 * we start IO but don't wait
		 */
		if (available < count)
			skel_do_read_io(dev, count - chunk);
	} else {
		/* no data in the buffer */
		rv = skel_do_read_io(dev, count);
		if (rv < 0)
			goto exit;
		else if (!(file->f_flags & O_NONBLOCK))
			goto retry;
		rv = -EAGAIN;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

static void skel_write_bulk_callback(struct urb *urb)
{
	struct usb_skel *dev;

	dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			err("%s - nonzero write bulk status received: %d",
			    __func__, urb->status);

		spin_lock(&dev->err_lock);
		dev->errors = urb->status;
		spin_unlock(&dev->err_lock);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
			  urb->transfer_buffer, urb->transfer_dma);
	up(&dev->limit_sem);
}

static ssize_t skel_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct usb_skel *dev;
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);

	dev = file->private_data;

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/*
	 * limit the number of URBs in flight to stop a user from using up all
	 * RAM
	 */
	if (!(file->f_flags & O_NONBLOCK)) {
		if (down_interruptible(&dev->limit_sem)) {
			retval = -ERESTARTSYS;
			goto exit;
		}
	} else {
		if (down_trylock(&dev->limit_sem)) {
			retval = -EAGAIN;
			goto exit;
		}
	}

	spin_lock_irq(&dev->err_lock);
	retval = dev->errors;
	if (retval < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&dev->err_lock);
	if (retval < 0)
		goto error;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, writesize, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}

	if (copy_from_user(buf, user_buffer, writesize)) {
		retval = -EFAULT;
		goto error;
	}

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&dev->io_mutex);
	if (!dev->interface) {		/* disconnect() was called */
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
			  buf, writesize, skel_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		err("%s - failed submitting write urb, error %d", __func__,
		    retval);
		goto error_unanchor;
	}

	/*
	 * release our reference to this urb, the USB core will eventually free
	 * it entirely
	 */
	usb_free_urb(urb);


	return writesize;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	if (urb) {
		usb_free_coherent(dev->udev, writesize, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}
	up(&dev->limit_sem);

exit:
	return retval;
}

static const struct file_operations skel_fops = {
	.owner =	THIS_MODULE,
	.read =		skel_read,
	.write =	skel_write,
	.open =		skel_open,
	.release =	skel_release,
	.flush =	skel_flush,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver skel_class = {
	.name =		"skel%d",
	.fops =		&skel_fops,
	.minor_base =	USB_SKEL_MINOR_BASE,
};

//系統會傳遞給探測函數一個usb_interface *跟一個struct usb_device_id *作為參數。
//他們分別是該USB設備的接口描述（一般會是該設備的第0號接口，
//該接口的默認設置也是第0號設置）跟它的設備ID描述（包括Vendor ID、Production ID等）
static int skel_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	/*
	probe函數主要完成以下工作:
	1.分配一個設備驅動空間，並對這個設備進行引用計數
	2.設備空間中的usb device,interface兩個成員，分別由真實成員來賦值，目的是把這些真實成員跟這個新的設備空間關聯起來
	3.判斷接口所包含的端點特性，看跟硬件是否一致，如果不一致，則出錯，即返回值不等於0
	4.註冊一個文件設備，這個文件設備是提供給應用程序用於操作設備的，包含打開，關閉，讀，寫等功能。
	5.成功則返回0
	*/

	// 建立一個 skeleton 並命名為一個很容易混淆的名子 -- dev
	// 其實對linux來說，usb_skel就是對應到 device( drive, device, bus)
	// 並且在這個init的地方，使用 kzalloc 配置記憶體
	struct usb_skel *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	printk(KERN_INFO "==eric_prob==,vid= %04x, pid=%04x\n", USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID );

	// 一個新的skeleton
	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err("Out of memory");
		goto error;
	}
	
	printk(KERN_ERR "devsize=%lx\n", sizeof(*dev));
	printk(KERN_ERR "dev=%x\n", dev);

	//初始化kref,把他設為1
	//這個是本module的kref, 至於usbDevice的kref是在 dev->dev->kref
	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_completion(&dev->bulk_in_completion);

	// 本來，要得到一個usb_device只要用interface_to_usbdev就夠了，
	// 但因為要增加對該usb_device的引用計數，我們應該在做一個usb_get_dev的操作，
	// 來增加引用計數，並在釋放設備時用usb_put_dev來減少引用計數
	// 該引用計數值是對該usb_device的計數，並不是對本模塊的計數
	// dev->dev->kref加1
	// 注意，第一個dev是代表usb skeleton，第二個才是usb_device
	dev->udev = usb_get_dev(interface_to_usbdev(interface));

	// interface定義在include/linux/usb.h中
	// 在linux中，一個USBinterface對應一種USB邏輯設備，比如鼠標、鍵盤、音頻流。
	// 所以，在USB範疇中，device一般就是指一個interface。一個驅動只控制一個interface。
	dev->interface = interface;

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */

	// 這邊要先提一下linux的習慣，各種descriptor都會有個叫 usb_host_xxx(如 usb_host_interface )
	// 而 usb_host_xxx 裡面會有一個叫 usb_xxx_descriptor(如usb_interface_descriptor)，命名通常以 desc 來命名
	// 裡面對應就是存放 device 回傳的 descriptor 的值
	
	// cur_altsetting為一種 usb_host_interface 的struct, 其中裡面的desc是一個叫 usb_interface_descriptor 的 struct，
	// mapping到ch9的interface_descriptor
	// 可以在include/linux/ch9.h中找到

	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		printk(KERN_ERR "dev->bulk_in_endpointAddr=%x\n", dev->bulk_in_endpointAddr);
		printk(KERN_ERR "bNumEndpoints=%x\n", iface_desc->desc.bNumEndpoints);
		printk(KERN_ERR "epNo=%d\n", i);
		showEndPoint(endpoint);

		// 把 device的endpoint descriptor，註冊到usb_skel中
		// usb_endpoint_is_bulk_in 是檢查 是否為 8xh(bulkin) 與屬性 attribule是否為0x02(代表bulk傳輸)
		if (!dev->bulk_in_endpointAddr &&
		    usb_endpoint_is_bulk_in(endpoint)) {
			/* we found a bulk in endpoint */

			// 根據device 回報的最大package size，來決定使用多少memory
			printk(KERN_ERR "We found bulkin\n");
			
			// usb_endpoint_maxp(endpoint) 其實就是 le16_to_cpu(epd->wMaxPacketSize);
			// le16_to_cpu 是前後MSB轉LSB顛倒, big_endlian和little_endian互轉
			buffer_size = usb_endpoint_maxp(endpoint);
			dev->bulk_in_size = buffer_size;
			dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;

			printk(KERN_ERR "buffer_size=%lx\n", buffer_size);
			printk(KERN_ERR "bulk_in_endpointAddr=%x\n", dev->bulk_in_endpointAddr);

			dev->bulk_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (!dev->bulk_in_buffer) {
				err("Could not allocate bulk_in_buffer");
				goto error;
			}

			// 使用 usb_alloc_urb 建立一個 urb
			// struct urb 可在include/linux/usb.h 找到
			dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
			if (!dev->bulk_in_urb) {
				err("Could not allocate bulk_in_urb");
				goto error;
			}
		}

		if (!dev->bulk_out_endpointAddr &&
		    usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
		}
	}

	//看看有沒有找到任何的bulk in /bulk out
	if (!(dev->bulk_in_endpointAddr && dev->bulk_out_endpointAddr)) {
		err("Could not find both bulk-in and bulk-out endpoints");
		goto error;
	}

	/* save our data pointer in this interface device */
	// usb_set_intfdata為一個inline function，在include/linux/usb.h中
	// 把向系統註冊，代表說，這個interface是使用這個usb_skel？
	// 之後可以利用usb_get_intfdata 把資料取出來
	
	//這邊其實是利用兩個inline funciton(一個在include/linux/usb.h, 另一個在include/linux/device.h)
	//把 interface->dev->driver_data = dev, 其實假設usbcore 保留的是interface的話，那麼把usb_skel放在interface就代表隨時可以取出來
	
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	//註冊 io 函數的 struct，會檢查&skel_class是否為NULL，同時也會配置主/次設備號
	retval = usb_register_dev(interface, &skel_class);
	if (retval) {
		/* something prevented us from registering this driver */
		err("Not able to get a minor for this device.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB Skeleton device now attached to USBSkel-%d",
		 interface->minor);
	return 0;

error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, skel_delete);
	return retval;
}

static void skel_disconnect(struct usb_interface *interface)
{
	printk(KERN_ERR "==eric_disconnect==\n");
	struct usb_skel *dev;
	
	int minor = interface->minor;

	//取出該interface所對應的 usb_skel( 該usb_skel在 probe階段被設定到interface上)
	dev = usb_get_intfdata(interface);

	//註銷該usb_skel( 利用set NULL的方式)
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	//註銷這個interface所綁定的 skel_class
	usb_deregister_dev(interface, &skel_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	//把kref引用計數減1，如果到0時，會呼叫skel_delete
	printk(KERN_INFO "eric_dev->kref = %d \n", dev->kref.refcount);
	kref_put(&dev->kref, skel_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);
}

static void skel_draw_down(struct usb_skel *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int skel_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	skel_draw_down(dev);
	return 0;
}

static int skel_resume(struct usb_interface *intf)
{
	return 0;
}

static int skel_pre_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	skel_draw_down(dev);

	return 0;
}

static int skel_post_reset(struct usb_interface *intf)
{
	struct usb_skel *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver skel_driver = {
	.name =		"skeleton",
	.probe =	skel_probe,
	.disconnect =	skel_disconnect,
	.suspend =	skel_suspend,
	.resume =	skel_resume,
	.pre_reset =	skel_pre_reset,
	.post_reset =	skel_post_reset,
	.id_table =	skel_table,
	.supports_autosuspend = 1,
};

static int __init usb_skel_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&skel_driver);
	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_skel_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&skel_driver);
}

module_init(usb_skel_init);
module_exit(usb_skel_exit);

MODULE_LICENSE("GPL");
