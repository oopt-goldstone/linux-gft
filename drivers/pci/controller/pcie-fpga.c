#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/string.h>
#include <linux/uaccess.h>
//#include <asm/uaccess.h>

//#include "fpga.h"

MODULE_LICENSE("Dual BSD/GPL");

#define FPGA_VENDOR 0x1957
#define FPGA_DEVICE 0xee01
#define FPGA_DEVICE1 0xe100
#define FPGA_DEVICE2 0xee02

#define DRIVER_NAME "Fpga Driver"

#define MAJNUM 99
#define DEVICE_COUNT 1

#define BAR_0 0

struct fpga {
	struct pci_dev* pci_dev;
	void* pci_regs;
	struct cdev char_dev;
	unsigned int offset;
};
static struct fpga fpga = {0};

static const struct pci_device_id fpga_pci_tbl[] = {
	{PCI_DEVICE(FPGA_VENDOR, FPGA_DEVICE)},
	{PCI_DEVICE(FPGA_VENDOR, FPGA_DEVICE1)},
	{PCI_DEVICE(FPGA_VENDOR, FPGA_DEVICE2)},
	{}
};
MODULE_DEVICE_TABLE(pci, fpga_pci_tbl);

// Used to retrieve data from the device.
ssize_t fpga_read(struct file * fl, char __user * buf, size_t count, loff_t * f_pos) {
	
	printk("Fpga: fpga_read(): read position: %lld count: %ld\n", *f_pos, count);
	
	// Make sure we're reading with our resource region
	if(*f_pos > pci_resource_len(fpga.pci_dev, BAR_0) - 4){
		printk("Fpga: fpga_read(): Error. Attempted to read outside of BAR region.\n");
		return -EINVAL;
	}
	
	// Limit reads to long.
	if(count > 4)
		count = 4;
	
	if (count == 4){
		unsigned long val = readl(fpga.pci_regs + *f_pos);
		copy_to_user(buf, &val, count);
	}
	else if (count == 2){
		unsigned short val = readw(fpga.pci_regs + *f_pos);
		copy_to_user(buf, &val, count);
	}
	else if(count == 1){
		unsigned char val = readb(fpga.pci_regs + *f_pos);
		copy_to_user(buf, &val, count);
	}else{
		printk("Fpga: fpga_read(): Invalid read count: %lld count: %ld\n", *f_pos, count);
		return -EINVAL;
	}
	
	*f_pos += count;
	
	return count;
	
}

ssize_t fpga_write(struct file *fl, const char __user *buf, size_t count, loff_t *f_pos){
	
	char kbuf[4];

	printk("Fpga: fpga_write(): write position: %lld count: %ld\n", *f_pos, count);

	// Make sure we're writing with our resource region
	if(*f_pos > pci_resource_len(fpga.pci_dev, BAR_0) - 4){
		printk("Fpga: fpga_write(): Error. Attempted to write outside of BAR region.\n");
		return -EINVAL;
	}

	// Limit reads to long.
	if(count > 4)
		count = 4;
	
	// Copy up to 4 bytes from user space buffer.
	copy_from_user(kbuf, buf, count);
	
	// Do the write depending on size.
	if (count == 4)
		writel(*(long*)buf, fpga.pci_regs + *f_pos);
	else if (count == 2)
		writew(*(short*)buf, fpga.pci_regs + *f_pos);
	else if(count == 1)
		writeb(*(char*)buf, fpga.pci_regs + *f_pos);
	else{
		printk("Fpga: fpga_write(): Invalid read count: %lld count: %ld\n", *f_pos, count);
		return -EINVAL;
	}

	*f_pos += count;

	return count;
}

loff_t fpga_llseek(struct file *filp, loff_t off, int from){
	
	// Save original position.
	unsigned long new_f_pos = 0;
	
	printk("Fpga: fpga_llseek(): %lld\n", off);
	
	// Seek from start of file.
	if(from == 0)
		new_f_pos = off;
	
	// Seek from current position
	else if (from == 1)
		new_f_pos = filp->f_pos + off;
		
	// Seek from end of file.
	else if (from == 2)
		new_f_pos = pci_resource_end(fpga.pci_dev, BAR_0) + off;
	
	// Should never happen
	else 
		return -EINVAL;
	
	// Verify that our new position doesn't allow reads outside of the resource.
	if(new_f_pos > pci_resource_len(fpga.pci_dev, BAR_0) - 4){
		printk("Fpga: fpga_llseek(): Error. Attempted to seek outside of BAR region.\n");
		return -EINVAL;
	}
	
	filp->f_pos = new_f_pos;
	return filp->f_pos;
}

struct file_operations fops = {
	.owner  = THIS_MODULE,
	.read   = fpga_read,
	.write  = fpga_write,
	.llseek = fpga_llseek
};

// Probe(), called once pci device is found.
static int fpga_init(struct pci_dev *pdev,  const struct pci_device_id *ent) {

	int error = 0;

	printk("Fpga: fpga_init(): Device found.\n");

	// Enable the PCI device.
	error = pci_enable_device(pdev);
	if(error){
		dev_err(&pdev->dev, "Fpga: fpga_init(): The  pci_enable_device() function failed\n");
		goto pci_enable_device_error;
	}

	// Reserve all the regions to our driver.
	error = pci_request_regions(pdev, DRIVER_NAME);
	if (error) {
		dev_err(&pdev->dev, "Fpga: fpga_init(): The pci_request_regions() function failed\n");
		goto pci_request_regions_error;
	}

	// Map BAR0 into our address space so we can access the registers
	fpga.pci_regs = pci_ioremap_bar(pdev, BAR_0);
	if(!fpga.pci_regs){
		dev_err(&pdev->dev, "Fpga: fpga_init(): Cannot map device registers, aborting\n");
		error = -1;
		goto pci_io_remap_bar_error;
	}
	
	// If needed, save the pci device in our fpga struct.
	fpga.pci_dev = pdev;
	
	// Now create our character device(s)
	
	// Register our desired device numbers.
	error = register_chrdev_region(MKDEV(MAJNUM, 0), DEVICE_COUNT, "fpga");
	if(error){
		printk("Fpga: fpga_init(): register_chrdev_region error.\n");
		goto register_chrdev_region_error;
	}
	
	// Initialize the char device.
   	cdev_init(&fpga.char_dev, &fops);
	fpga.char_dev.owner = THIS_MODULE;
	
	// Add the char device to the system, making the device live.
	error = cdev_add(&fpga.char_dev, MKDEV(MAJNUM, 0), DEVICE_COUNT);
	if(error){
		printk("Fpga: fpga_init(): cdev_add failed\n");
		goto cdev_add_error;
	}
	
	// Return zero, indicating successful claim.
	return 0;

// Initialization unwinding.
	
// Unregister the char device number region.
cdev_add_error:
	unregister_chrdev_region(MKDEV(MAJNUM, 0), DEVICE_COUNT);

// Unmap the bar.
register_chrdev_region_error:
	iounmap(fpga.pci_regs);

// Undo the pci region request.
pci_io_remap_bar_error:
	pci_release_regions(pdev);

// Undo the PCI enabling
pci_request_regions_error:
	pci_disable_device(pdev);
	
//Nothing to undo here.
pci_enable_device_error:
	return error;
}

//Undo the fpga pci initialization.
static void fpga_remove(struct pci_dev *pdev) {
	printk("Fpga: fpga_remove(): Removing driver...\n");
	
	// Delete and unregister the char device(s).
	cdev_del(&fpga.char_dev);
	unregister_chrdev_region(MKDEV(MAJNUM, 0), DEVICE_COUNT);
	
	// Remove the PCI device.
	iounmap(fpga.pci_regs);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	fpga.pci_regs = 0;
	fpga.pci_dev  = 0;
}

static struct pci_driver fpga_pci_driver = {
	.name		= DRIVER_NAME,
	.id_table	= fpga_pci_tbl,
	.probe		= fpga_init,
	.remove		= fpga_remove,
};

module_pci_driver(fpga_pci_driver)

