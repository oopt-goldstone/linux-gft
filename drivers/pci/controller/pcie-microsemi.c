
/*
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
*/
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/uaccess.h>

static char ids[1024] __initdata;

module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
					  "\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
					  " and multiple comma separated entries can be specified");

#define FPGA_VENDOR 0x1957
#define FPGA_DEVICE 0xee01
#define DRIVER_NAME "pci-char"

static const struct pci_device_id fpga_pci_tbl[] = {
	{PCI_DEVICE(FPGA_VENDOR, FPGA_DEVICE)},
	{0}};

/* Base Address register */
struct bar_t
{
	resource_size_t len;
	void __iomem *addr;
};

/* Private structure */
struct pci_char
{
	struct bar_t bar[6];
	dev_t major;
	struct cdev cdev;
};

static struct class *pchar_class;

static int dev_open(struct inode *inode, struct file *file)
{
	unsigned int num = iminor(file->f_path.dentry->d_inode);
	struct pci_char *pchar = container_of(inode->i_cdev, struct pci_char,
										  cdev);
	//pr_info("%s %d, %d, %d", __FUNCTION__, __LINE__, num, pchar->bar[num].len);
	if (num > 5)
		return -ENXIO;

	if (pchar->bar[num].len == 0)
		return -EIO; /* BAR not in use or not memory type */

	file->private_data = pchar;
	return 0;
};

static loff_t dev_seek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	unsigned int num = iminor(inode);
	struct pci_char *pchar = file->private_data;
	loff_t new_pos;
	pr_info("%s %d", __FUNCTION__, __LINE__);

	//mutex_lock(&inode->i_mutex);
	inode_lock(inode);
	switch (whence)
	{
	case SEEK_SET: /* SEEK_SET = 0 */
		new_pos = offset;
		break;
	case SEEK_CUR: /* SEEK_CUR = 1 */
		new_pos = file->f_pos + offset;
		break;
	default:
		new_pos = -EINVAL;
	}
	//mutex_unlock(&inode->i_mutex);
	inode_unlock(inode);

	if (new_pos % 4)
		return -EINVAL; /* Only allow 4 byte alignment */

	if ((new_pos < 0) || (new_pos > pchar->bar[num].len - 4))
		return -EINVAL;

	file->f_pos = new_pos;
	return file->f_pos;
}

static ssize_t dev_read(struct file *file, char __user *buf,
						size_t count, loff_t *ppos)
{
	struct pci_char *pchar = file->private_data;
	u32 __user *tmp = (u32 __user *)buf;
	u32 data;
	u32 offset = *ppos;
	unsigned int num = iminor(file->f_path.dentry->d_inode);
	int err = 0;
	ssize_t bytes = 0;
	pr_info("%s %d", __FUNCTION__, __LINE__);
	if (count % 4)
		return -EINVAL; /* Only allow 32 bit reads */

	for (; count; count -= 4)
	{
		data = readl(pchar->bar[num].addr + offset);
		if (copy_to_user(tmp, &data, 4))
		{
			err = -EFAULT;
			break;
		}
		tmp += 1;
		bytes += 4;
	}

	return bytes ? bytes : err;
};

static ssize_t dev_write(struct file *file, const char __user *buf,
						 size_t count, loff_t *ppos)
{
	struct pci_char *pchar = file->private_data;
	const u32 __user *tmp = (const u32 __user *)buf;
	u32 data;
	u32 offset = *ppos;
	unsigned int num = iminor(file->f_path.dentry->d_inode);
	int err = 0;
	ssize_t bytes = 0;
	pr_info("%s %d", __FUNCTION__, __LINE__);
	if (count % 4)
		return -EINVAL; /* Only allow 32 bit writes */

	for (; count; count -= 4)
	{
		if (copy_from_user(&data, tmp, 4))
		{
			err = -EFAULT;
			break;
		}
		writel(data, pchar->bar[num].addr + offset);
		tmp += 1;
		bytes += 4;
	}

	return bytes ? bytes : err;
};

static const struct file_operations fops = {
	.owner	 = THIS_MODULE,
	.llseek  = dev_seek,
	.open	 = dev_open,
	.read	 = dev_read,
	.write	 = dev_write,
};

static int pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int err = 0, i;
	int mem_bars;
	struct pci_char *pchar;
	struct device *dev;
	dev_t dev_num;
	pr_info("%s %d", __FUNCTION__, __LINE__);
	pchar = kzalloc(sizeof(struct pci_char), GFP_KERNEL);
	if (!pchar)
	{
		err = -ENOMEM;
		goto failure_kmalloc;
	}

	err = pci_enable_device_mem(pdev);
	if (err)
		goto failure_pci_enable;

	/* Request only the BARs that contain memory regions */
	mem_bars = pci_select_bars(pdev, IORESOURCE_MEM);
	err = pci_request_selected_regions(pdev, mem_bars, DRIVER_NAME);
	//err = pci_request_regions(pdev, DRIVER_NAME);
	pr_info("%s %d", __FUNCTION__, __LINE__);
	if (err)
		goto failure_pci_regions;

	/* Memory Map BARs for MMIO */
	pr_info("%s %d", __FUNCTION__, __LINE__);
	for (i = 0; i < 6; i++)
	{
		if (mem_bars & (1 << i))
		{
			//pchar->bar[i].addr = pci_ioremap_bar(pdev, i);
			pchar->bar[i].addr = ioremap(pci_resource_start(pdev, i),
										 pci_resource_len(pdev, i));
			if (IS_ERR(pchar->bar[i].addr))
			{
				err = PTR_ERR(pchar->bar[i].addr);
				break;
			}
			else
				pchar->bar[i].len = pci_resource_len(pdev, i);
		}
		else
		{
			pchar->bar[i].addr = NULL;
			pchar->bar[i].len = 0;
		}
	}

	if (err)
	{
		for (i--; i >= 0; i--)
			if (pchar->bar[i].len)
				iounmap(pchar->bar[i].addr);
		goto failure_ioremap;
	}

	/* Get device number range */
	err = alloc_chrdev_region(&dev_num, 0, 6, DRIVER_NAME);
	if (err)
		goto failure_alloc_chrdev_region;

	pchar->major = MAJOR(dev_num);
	pr_info("%s %d", __FUNCTION__, __LINE__);
	/* connect cdev with file operations */
	cdev_init(&pchar->cdev, &fops);
	pchar->cdev.owner = THIS_MODULE;

	/* add major/min range to cdev */
	err = cdev_add(&pchar->cdev, MKDEV(pchar->major, 0), 6);
	if (err)
		goto failure_cdev_add;

	/* create /dev/ nodes via udev */
	for (i = 0; i < 6; i++)
	{
		if (pchar->bar[i].len)
		{
			// err = cdev_add(&pchar->cdev, MKDEV(pchar->major, i), 1);
			// if (err)
			// 	goto failure_cdev_add;
			dev = device_create(pchar_class, &pdev->dev,
								MKDEV(pchar->major, i),
								NULL, "b%xd%xf%x_bar%d",
								pdev->bus->number,
								PCI_SLOT(pdev->devfn),
								PCI_FUNC(pdev->devfn), i);
			if (IS_ERR(dev))
			{
				err = PTR_ERR(dev);
				break;
			}
		}
	}

	if (err)
	{
		for (i--; i >= 0; i--)
			if (pchar->bar[i].len)
				device_destroy(pchar_class, MKDEV(pchar->major, i));
		goto failure_device_create;
	}

	pr_info("%s %d", __FUNCTION__, __LINE__);
	pci_set_drvdata(pdev, pchar);
	dev_info(&pdev->dev, "claimed by pci-char\n");

	return 0;

failure_device_create:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	cdev_del(&pchar->cdev);

failure_cdev_add:
	unregister_chrdev_region(MKDEV(pchar->major, 0), 6);

failure_alloc_chrdev_region:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			iounmap(pchar->bar[i].addr);

failure_ioremap:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	pci_release_selected_regions(pdev,
								 pci_select_bars(pdev, IORESOURCE_MEM));

failure_pci_regions:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	pci_disable_device(pdev);

failure_pci_enable:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	kfree(pchar);

failure_kmalloc:
	return err;
}

static void pci_remove(struct pci_dev *pdev)
{
	int i;
	struct pci_char *pchar = pci_get_drvdata(pdev);

	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			device_destroy(pchar_class,
						   MKDEV(pchar->major, i));

	cdev_del(&pchar->cdev);

	unregister_chrdev_region(MKDEV(pchar->major, 0), 6);

	for (i = 0; i < 6; i++)
		if (pchar->bar[i].len)
			iounmap(pchar->bar[i].addr);

	pci_release_selected_regions(pdev,
								 pci_select_bars(pdev, IORESOURCE_MEM));
	pci_disable_device(pdev);
	kfree(pchar);
}

static struct pci_driver pchar_driver = {
	.name		= DRIVER_NAME,
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_probe,
	.remove		= pci_remove,
};

static char *pci_char_devnode(struct device *dev, umode_t *mode)
{
	struct pci_dev *pdev = to_pci_dev(dev->parent);
	return kasprintf(GFP_KERNEL, "pci-char/%02x:%02x.%02x/bar%d",
					 pdev->bus->number,
					 PCI_SLOT(pdev->devfn),
					 PCI_FUNC(pdev->devfn),
					 MINOR(dev->devt));
}

static int __init pci_init(void)
{
	int err;
	char *p, *id;

	pchar_class = class_create(THIS_MODULE, DRIVER_NAME);
	if (IS_ERR(pchar_class))
	{
		err = PTR_ERR(pchar_class);
		return err;
	}
	pchar_class->devnode = pci_char_devnode;
	pr_info("%s %d", __FUNCTION__, __LINE__);
	err = pci_register_driver(&pchar_driver);
	if (err)
		goto failure_register_driver;
	pr_info("%s %d", __FUNCTION__, __LINE__);
	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	 p = ids;
	while ((id = strsep(&p, ",")))
	{
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
									 subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		int fields;
		pr_info("%s %d",__FUNCTION__,__LINE__);
		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
						&vendor, &device, &subvendor, &subdevice,
						&class, &class_mask);

		if (fields < 2)
		{
			pr_warn("pci-char: invalid id string \"%s\"\n", id);
			continue;
		}

		pr_info("pci-char: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
				vendor, device, subvendor, subdevice, class, class_mask);

		err = pci_add_dynid(&pchar_driver, vendor, device,
							subvendor, subdevice, class, class_mask, 0);
		if (err)
			pr_warn("pci-char: failed to add dynamic id (%d)\n", err);
	}
 
	return 0;

failure_register_driver:
	pr_info("%s %d", __FUNCTION__, __LINE__);
	class_destroy(pchar_class);

	return err;
}

static void __exit pci_exit(void)
{
	pci_unregister_driver(&pchar_driver);
	class_destroy(pchar_class);
}

module_init(pci_init);
module_exit(pci_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("generic pci to chardev driver");
