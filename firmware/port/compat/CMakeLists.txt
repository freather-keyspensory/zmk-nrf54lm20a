zephyr_include_directories(include)
zephyr_syscall_header(${CMAKE_CURRENT_LIST_DIR}/include/zephyr/drivers/kscan.h)
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MINEW_MANUAL_KEY manual_key.c)

if(CONFIG_ZMK_USB_NEXT)
  zephyr_library_include_directories(${ZEPHYR_BASE}/samples/subsys/usb/common)
  zephyr_library_sources(usb_next.c ${ZEPHYR_BASE}/samples/subsys/usb/common/sample_usbd_init.c)
endif()
