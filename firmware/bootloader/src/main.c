/* SPDX-License-Identifier: MIT */
#include <sample_usbd.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/storage/disk_access.h>
#include <errno.h>
#include "uf2_draft.h"
#ifndef MINEW_MCUBOOT
static struct uf2_session session;
#else
extern int minew_receive_block(const uint8_t *block);
#endif
static int init(struct disk_info *d){ARG_UNUSED(d);return 0;}
static int read_disk(struct disk_info *d,uint8_t *b,uint32_t start,uint32_t n){
 ARG_UNUSED(d);if(start>=UF2_DISK_SECTORS || n>UF2_DISK_SECTORS-start)return -EINVAL;
 for(uint32_t i=0;i<n;i++) { uf2_disk_read(start+i,b+512*i); }
 return 0;
}
static int write_disk(struct disk_info *d,const uint8_t *b,uint32_t start,uint32_t n){
 ARG_UNUSED(d);if(start>=UF2_DISK_SECTORS || n>UF2_DISK_SECTORS-start)return -EINVAL;
 for(uint32_t i=0;i<n;i++){
  #ifdef MINEW_MCUBOOT
  int r=minew_receive_block(b+512*i);
#else
  int r=uf2_accept(&session,b+512*i);
#endif
  if(r<0)printk("UF2 rejected: %d\n",r);
  #ifndef MINEW_MCUBOOT
  if(r==1 && session.received==session.total)
   printk("UF2 structurally complete: %u blocks. DRY RUN: NOT INSTALLED\n",session.total);
#endif
 }
 /* FAT/metadata writes are acknowledged but not retained, as on a virtual UF2 disk. */
 return 0;
}
static int ioctl_disk(struct disk_info *d,uint8_t cmd,void *p){
 ARG_UNUSED(d);
 switch(cmd){
 case DISK_IOCTL_CTRL_INIT:case DISK_IOCTL_CTRL_DEINIT:case DISK_IOCTL_CTRL_SYNC:return 0;
 case DISK_IOCTL_GET_SECTOR_COUNT:*(uint32_t*)p=UF2_DISK_SECTORS;return 0;
 case DISK_IOCTL_GET_SECTOR_SIZE:*(uint32_t*)p=512;return 0;
 case DISK_IOCTL_GET_ERASE_BLOCK_SZ:*(uint32_t*)p=1;return 0;
 default:return -EINVAL;
 }
}
static const struct disk_operations ops={.init=init,.status=init,.read=read_disk,.write=write_disk,.ioctl=ioctl_disk};
static struct disk_info disk={.name="UF2",.ops=&ops};
USBD_DEFINE_MSC_LUN(uf2,"UF2","Minew","UF2 Draft","0.01");
#ifdef MINEW_MCUBOOT
int minew_usb_init(void){
#else
int main(void){
#endif
 int r=disk_access_register(&disk);if(r)return r;
 struct usbd_context *usb=sample_usbd_init_device(NULL);
 if(!usb)return -ENODEV;
 r=usbd_enable(usb);printk("Minew UF2 USB enable: %d\n",r);
 return r;
}
