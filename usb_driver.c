#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/usb.h>


static int skel_probe(void)
{
}
static int skel_disconnect(void)
{
}

#define USB_SKEL_VENDOR_ID  0xfff0
#define USB_SKEL_PRODUCT_ID 0xfff0
/* table of devices that work with this driver */
static struct usb_device_id skel_table [] = {
     { USB_DEVICE(USB_SKEL_VENDOR_ID, USB_SKEL_PRODUCT_ID) },
     { }                    /* Terminating entry */
};
MODULE_DEVICE_TABLE (usb, skel_table); 




static struct usb_driver skel_driver = {
     .name =      "skeleton",
     .probe =     skel_probe,
     .disconnect = skel_disconnect,
     .id_table =    skel_table,
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

module_init (usb_skel_init);
module_exit (usb_skel_exit);
MODULE_LICENSE("GPL"); 






