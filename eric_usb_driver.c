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

/* table of devices that work with this driver */
static const struct usb_device_id skel_table[] = {
	{ USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

//告訴用戶空間的熱插拔和模塊裝載，vid和pid對應什麼硬件設備。以便執行自動掛載。
MODULE_DEVICE_TABLE(usb, skel_table);
	
static int __init usb_skel_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&skel_driver);

	printk(KERN_INFO "eric_inti_res= %d\n", result);

	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_skel_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&skel_driver);

	printk(KERN_INFO "eric_usb_skel_exit\n");
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

	printk(KERN_INFO "eric_skel_delete\n");
}

static void skel_disconnect(struct usb_interface *interface)
{
	struct usb_skel *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &skel_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, skel_delete);

	dev_info(&interface->dev, "USB Skeleton #%d now disconnected", minor);

	printk(KERN_INFO "eric_skel_disconnect\n");
}

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
	printk(KERN_INFO "eric_skel_probe\n");
	return -1;
}

static struct usb_driver skel_driver = {
	.name =		"eric_usb_driver_test",
	.probe =	skel_probe,
	.disconnect =	skel_disconnect,
	.id_table =	skel_table,
};

