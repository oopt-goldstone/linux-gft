#include <linux/kernel.h>			/* Need for KERN_INFO */
#include <linux/version.h>
#include <linux/module.h>			/* Needed by all modules */
#include <linux/fs.h>
#include <linux/init.h>			/* Need for the macros */
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <asm/current.h>
#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/bitops.h>
#include <linux/phy.h>
#include <linux/mutex.h>

#include "enetc.h"


#define MDIO_DEV_NAME "ENETC_MDIO_DEV"

#define MPCI_IOC_MAGIC  'm'

#define ENETC_MDIO_WRITE    		_IOWR(MPCI_IOC_MAGIC, 0, int)
#define ENETC_MDIO_READ 		_IOWR(MPCI_IOC_MAGIC, 1, int)

#define SUCCESS 0L
#define IS_C45 1

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;


struct config_mdio {

	bool is_C45;
	u32 phy_addr;
	u32 dev_addr;
	u32 reg_addr;
	u16 value;

};



struct cdev *enetc_mdio_cdev = NULL;
dev_t enetc_mdio_dev;
static struct class *enetc_mdio_class = NULL;


/**
* char_dev_open() - Open Char Device
*
*  @inode : Device Driver Information
*  @file  : Pointer to the file struct
*/
static int char_mdio_dev_open(struct inode* inode,
	struct file* file)
{
	printk("open operation invoked \n");
	return SUCCESS;
}

/**
* char_dev_release() - Release Char Device
*
*  @inode : Device Driver Information
*  @file  : Pointer to the file struct
*/

static int char_mdio_dev_release(struct inode* inode,
	struct file* file)
{
	printk("close operation invoked \n");
	return SUCCESS;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
static int char_mdio_dev_ioctl(struct inode* i, struct file* f, u32 cmd, unsigned long arg)
#else
static long char_mdio_dev_ioctl(struct file* f, u32 cmd, unsigned long arg)
#endif
{
	int ret = 0;
	int regnum = 0;
	struct config_mdio local_cf;

	if (copy_from_user(&local_cf, (struct config_mdio *)arg, sizeof(struct config_mdio)))
	{
		return -EACCES;
	}

	if(local_cf.is_C45 == IS_C45)
	{
		regnum = MII_ADDR_C45 | local_cf.dev_addr << 16 | local_cf.reg_addr;
	}
	else
	{
		regnum = local_cf.reg_addr;
	}

	printk("in ENETC_MDIO phy address = %8x \n", local_cf.phy_addr);
	printk("in ENETC_MDIO regnum = %8x \n", regnum);
	printk("in ENETC_MDIO value    = %4x \n", local_cf.value);
	
	switch (cmd)
	{
	case ENETC_MDIO_WRITE:
		printk("in ENETC_MDIO_WRITE \n");
		
		mutex_lock(&mdio_demo->mdio_lock);
		printk("mdio write start... \n");
		ret = mdio_demo->write(mdio_demo, local_cf.phy_addr, regnum, local_cf.value);
		printk("mdio write end... \n");
		mutex_unlock(&mdio_demo->mdio_lock);
		printk("mutex_unlock \n");
		
		break;
	case ENETC_MDIO_READ:
		printk("in ENETC_MDIO_READ \n");
		
		mutex_lock(&mdio_demo->mdio_lock);
		printk("mdio read start... \n");
		local_cf.value = mdio_demo->read(mdio_demo, local_cf.phy_addr, regnum);
		printk("mdio read end... \n");
		mutex_unlock(&mdio_demo->mdio_lock);
		printk("mutex_unlock \n");

		printk("in ENETC_MDIO_READ read value    = %4x \n", local_cf.value);
		ret = copy_to_user((struct config_mdio*)arg, &local_cf, sizeof(struct config_mdio));
		printk(KERN_INFO "ret = %d\n", ret);

		break;
	}

	
	return ret;
}




static struct file_operations char_mdio_dev_fops = {
	.owner = THIS_MODULE,
	.open = char_mdio_dev_open,
	.release = char_mdio_dev_release,
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,35))
   	.ioctl = char_mdio_dev_ioctl,
#else
    .unlocked_ioctl = char_mdio_dev_ioctl,
#endif
};

/**
*  pcidrv_init() - Initialize PCIe Module
*
*  Registers Platform Driver
*/

static __init int mdiodrv_init(void)
{
	int ret,count=1;

	printk("Module Inserted \n");

	/* Request dynamic allocation of a device major number */
	if(alloc_chrdev_region (&enetc_mdio_dev, 0, count, MDIO_DEV_NAME) < 0) {
    printk (KERN_ERR "failed to reserve major/minor range\n");
    return -1;
		}

	printk("ENETC_MDIO_DEV:  Got Major %d\n", MAJOR(enetc_mdio_dev));

	if (!(enetc_mdio_cdev = cdev_alloc ())) {
    printk (KERN_ERR "cdev_alloc() failed\n");
    unregister_chrdev_region (enetc_mdio_dev, count);
    return -1;
	}
	/* Connect the file operations with cdev*/
	cdev_init(enetc_mdio_cdev,&char_mdio_dev_fops);

	/* Connect the majot/mionr number to the cdev */
	ret=cdev_add(enetc_mdio_cdev,enetc_mdio_dev,count);
	if( ret < 0 ) {
		printk("Error registering device driver\n");
		cdev_del(enetc_mdio_cdev);
		unregister_chrdev_region(enetc_mdio_dev, count);
		return -1;
	}
	/* Populate sysfs entry */
	enetc_mdio_class = class_create(THIS_MODULE, "enetc_mdio");

	/* Send uevents to udev, So it will create /dev nodes */
	device_create(enetc_mdio_class, NULL, enetc_mdio_dev, "%s", MDIO_DEV_NAME);

	printk("Successfully created /dev/ENETC_MDIO_DEV\n");

	printk("\nDevice Registered: %s\n", MDIO_DEV_NAME);
	printk(KERN_INFO "Major number = %d, Minor number = %d\n", MAJOR(enetc_mdio_dev), MINOR(enetc_mdio_dev));

	return 0;
}

/**
* pcidrv_cleanup() - Uninitialize the PCIe Module
*
*  Unregister the Platform Driver
*/

static __exit void  mdiodrv_cleanup(void)
{
	printk("Module Deleted \n");
	/* Remove the cdev */
	cdev_del(enetc_mdio_cdev);
	/* Release the major number */
	unregister_chrdev_region(enetc_mdio_dev, 1);
	/* Destroy device and class */
	device_destroy(enetc_mdio_class, enetc_mdio_dev);
	class_destroy(enetc_mdio_class);

	printk("\n Driver unregistered \n");
}

module_init(mdiodrv_init);
module_exit(mdiodrv_cleanup);

MODULE_AUTHOR("Wistron");
MODULE_DESCRIPTION("Userspace Mdio Driver");
MODULE_LICENSE("GPL");


