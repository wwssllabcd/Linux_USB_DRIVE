#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>


static struct usb_driver skel_driver;
static struct usb_class_driver skel_class;

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

static int __init usb_skel_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	//註冊本驅動，可用lsmod觀察
	result = usb_register(&skel_driver);
	printk(KERN_INFO "eric_init= %d, vid= %04x, pid=%04x\n", result, USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID );

	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_skel_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&skel_driver);

	printk(KERN_ERR "eric_usb_skel_exit\n");
}

module_init(usb_skel_init);
module_exit(usb_skel_exit);
MODULE_LICENSE("GPL");

#define to_skel_dev(d) container_of(d, struct usb_skel, kref)


static void skel_delete(struct kref *kref)
{
	//skel_delete主要作用就是减"1"
	/*
	1.獲得設備
	2.減少設備的引用次數
	3.釋放分配的數據空間
	4.釋放分配的驅動空間
	*/
	
	struct usb_skel *dev = to_skel_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	
	//釋放批量輸入端口緩衝
	kfree(dev->bulk_in_buffer);
	
	//釋放設備
	kfree(dev);
	
	printk(KERN_ERR "eric_skel_delete\n");
}

static void skel_disconnect(struct usb_interface *interface)
{
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

	printk(KERN_ERR "eric_skel_disconnect\n");
}


static int skel_open(struct inode *inode, struct file *file)
{
	struct usb_skel *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);
	
	printk(KERN_ERR "eric_open, subminor=%d\n", subminor);

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
	
	printk(KERN_ERR "refcount=%d\n", dev->kref->refcount);

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

	printk(KERN_ERR "open_count=%d, retval=%d\n", dev->open_count, retval);
	
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
	
	printk(KERN_ERR "eric_skel_release\n");
	return 0;
}

static int skel_flush(struct file *file, fl_owner_t id)
{
	int res = 0 ;
	
	printk(KERN_ERR "eric_skel_flush\n");
	return res;
}

static ssize_t skel_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	int rv=0;
	
	printk(KERN_ERR "eric_skel_read\n");
	return rv;
}

static ssize_t skel_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	int retval = 0;
	
	printk(KERN_ERR "eric_skel_write\n");
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
	.name =		"usb_eric_%d",                // .name 對應到的是掛載後，/dev下面出現的名稱
	.fops =		&skel_fops,
	.minor_base =	USB_SKEL_MINOR_BASE, //主設備號用來區分不同類型的設備，而次設備號用來區分同一類型內的多個設備。
};


static void showEndPoint(const struct usb_endpoint_descriptor *endpoint)
{
	printk(KERN_ERR "ep->bLength=%x\n", endpoint->bLength);
	printk(KERN_ERR "ep->bDescriptorType=%x\n", endpoint->bDescriptorType);
	printk(KERN_ERR "ep->bEndpointAddress=%x\n", endpoint->bEndpointAddress);
	printk(KERN_ERR "ep->bmAttributes=%x\n", endpoint->bmAttributes);
	printk(KERN_ERR "ep->wMaxPacketSize=%x\n", endpoint->wMaxPacketSize);
	printk(KERN_ERR "ep->bInterval=%x\n", endpoint->bInterval);
}

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

	printk(KERN_ERR "eric_probe, ret=%d\n", retval);

	// 一個新的skeleton
	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err("Out of memory");
		goto error;
	}

	printk(KERN_ERR "eric_probe, devsize=%lx\n", sizeof(*dev));
	printk(KERN_ERR "eric_probe, dev=%x\n", dev);

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

		printk(KERN_ERR "eric_probe, dev->bulk_in_endpointAddr=%x\n", dev->bulk_in_endpointAddr);
		printk(KERN_ERR "bNumEndpoints=%x\n", iface_desc->desc.bNumEndpoints);

		printk(KERN_ERR "eric_probe, epNo=%d\n", i);
		showEndPoint(endpoint);


		// 把 device的endpoint descriptor，註冊到usb_skel中
		// usb_endpoint_is_bulk_in 是檢查 是否為 8xh(bulkin) 與屬性 attribule是否為0x02(代表bulk傳輸)
		if (!dev->bulk_in_endpointAddr && usb_endpoint_is_bulk_in(endpoint)) {
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

		if (!dev->bulk_out_endpointAddr && usb_endpoint_is_bulk_out(endpoint)) {
			
			/* we found a bulk out endpoint */
			dev->bulk_out_endpointAddr = endpoint->bEndpointAddress;
			printk(KERN_ERR "We found bulkout = %x\n", dev->bulk_out_endpointAddr );
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
	dev_info(&interface->dev, "eric usb device now attached to USBSkel-%d", interface->minor);
	return 0;

error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, skel_delete);
	return retval;
}

static struct usb_driver skel_driver = {
	.name =		"eric_usb_driver_test",
	.probe =	skel_probe,
	
	//設備被拔出集線器時，usb子系統會自動地調用disconnect
	.disconnect =	skel_disconnect,  
	.id_table =	skel_table,
};

