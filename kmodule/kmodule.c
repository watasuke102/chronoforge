#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static long module_ioctl(
    struct file* file, unsigned int cmd, unsigned long arg) {
  printk(KERN_INFO "ioctl: %d", cmd);
  return 0;
}

static int module_open(struct inode* inode, struct file* file) {
  return 0;
}
static int module_release(struct inode* inode, struct file* file) {
  return 0;
}

static struct file_operations ops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = module_ioctl,
    .open           = module_open,
    .release        = module_release,
};
static struct cdev cdev;

static int module_entry(void) {
  dev_t devno;
  int   ret = alloc_chrdev_region(&devno, 0, 1, "kmodule");
  if (ret) {
    printk(KERN_ERR "Failed to register character device region (%d)\n", ret);
    return -1;
  }
  cdev_init(&cdev, &ops);
  ret = cdev_add(&cdev, devno, 1);
  if (ret) {
    printk(KERN_ERR "Failed to add character device (%d)\n", ret);
    return -1;
  }
  printk(KERN_INFO "Module initialized successfully\n");
  return 0;
}
static void module_cleanup(void) {
  cdev_del(&cdev);
  unregister_chrdev_region(cdev.dev, 1);
  printk(KERN_INFO "Module exited successfully\n");
}

module_init(module_entry);
module_exit(module_cleanup);

MODULE_LICENSE("MIT");
