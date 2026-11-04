/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <bootutil/bootutil.h>
#include <bootutil/bootutil_public.h>
#include <bootutil/boot_hooks.h>
#include <bootutil/image.h>
#include <errno.h>
#include <string.h>
#include "uf2_draft.h"

/* One MSC worker owns session and staging. The boot thread reads only phase. */
static struct uf2_session session;
static atomic_t phase;
enum { IDLE, RECEIVING, READY, FAILED };
static const struct flash_area *secondary;
extern int minew_usb_init(void);

int minew_receive_block(const uint8_t *block)
{
 int state=atomic_get(&phase);
 if(state==FAILED)return -EIO;
 if(state==READY)return 0;
 int r=uf2_accept(&session,block);
 if(r<0){atomic_set(&phase,FAILED);return r;}
 if(r!=1)return 0;
 if(state==IDLE){
  atomic_set(&phase,RECEIVING);
  r=flash_area_open(PARTITION_ID(slot1_partition),&secondary);
  /* Reserve the complete final sector for MCUboot's trailer. */
  if(r || session.total>(secondary->fa_size-4096)/256){atomic_set(&phase,FAILED);return -EFBIG;}
  r=flash_area_flatten(secondary,0,secondary->fa_size);
  if(r){atomic_set(&phase,FAILED);return r;}
 }
 uint32_t n=sys_get_le32(block+20), offset=n*256;
 r=flash_area_write(secondary,offset,block+32,256);
 uint8_t verify[256];
 if(!r)r=flash_area_read(secondary,offset,verify,sizeof(verify));
 if(!r && memcmp(verify,block+32,256))r=-EIO;
 if(r){atomic_set(&phase,FAILED);return r;}
 if(session.received==session.total){
  struct image_header h;
  r=flash_area_read(secondary,0,&h,sizeof(h));
  uint32_t received=session.total*256;
  if(r || h.ih_magic!=IMAGE_MAGIC || h.ih_hdr_size!=2048 ||
     received<h.ih_hdr_size || !h.ih_img_size || h.ih_img_size>received-h.ih_hdr_size){
   atomic_set(&phase,FAILED);return -EINVAL;
  }
  /* Pending is not trusted or active: boot_go validates the signature before swap. */
  r=boot_set_pending(0);
  if(r){atomic_set(&phase,FAILED);return r;}
  flash_area_close(secondary);secondary=NULL;
  atomic_set(&phase,READY);
  printk("MINEW UF2 staged %u blocks; reboot for MCUboot signature verification\n",session.total);
 }
 return 0;
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
 const struct device *retained=DEVICE_DT_GET(DT_NODELABEL(gpregret1));
 uint8_t request=0,marker=0x57,clear=0;
 if(device_is_ready(retained)){
  retained_mem_read(retained,0,&request,1);
  retained_mem_write(retained,0,&marker,1);
 }
 /* A second reset within this window requests recovery. */
 k_msleep(700);
 if(device_is_ready(retained))retained_mem_write(retained,0,&clear,1);
 fih_ret result;
 FIH_CALL(boot_go,result,rsp);
 if(FIH_EQ(result,FIH_SUCCESS) && request!=0x57 && request!=0xb1){
  FIH_RET(result);
 }
 printk("MINEW UF2 recovery: request=%02x, valid application=%d\n",request,FIH_EQ(result,FIH_SUCCESS));
 if(minew_usb_init()){
  /* Do not bypass validation or jump to an invalid image if USB init fails. */
  if(FIH_EQ(result,FIH_SUCCESS)){FIH_RET(result);}
  for(;;)k_msleep(1000);
 }
 for(;;){
  if(atomic_get(&phase)==READY){k_msleep(2000);sys_reboot(SYS_REBOOT_COLD);}
  k_msleep(50);
 }
}
