#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

static int module_entry(void) {
  printk(KERN_INFO "Module initialized successfully.\n");
  return 0;
}
static void module_cleanup(void) {
  printk(KERN_INFO "Module exited successfully.\n");
}

module_init(module_entry);
module_exit(module_cleanup);

MODULE_LICENSE("MIT");
