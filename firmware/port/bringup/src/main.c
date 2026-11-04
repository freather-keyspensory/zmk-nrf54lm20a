#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
int main(void) {
 printk("MINEW MX54BE13 bringup boot\n");
 unsigned int heartbeat = 0;
 while (1) {
  printk("MINEW heartbeat %u uptime %lld\n", heartbeat++, k_uptime_get());
  k_sleep(K_SECONDS(2));
 }
 return 0;
}
