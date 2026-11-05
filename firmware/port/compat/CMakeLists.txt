zephyr_include_directories(include)
zephyr_syscall_header(${CMAKE_CURRENT_LIST_DIR}/include/zephyr/drivers/kscan.h)
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MINEW_MANUAL_KEY manual_key.c)

if(CONFIG_ZMK_USB_NEXT)
  zephyr_library_include_directories(${ZEPHYR_BASE}/samples/subsys/usb/common)
  zephyr_library_sources(usb_next.c ${ZEPHYR_BASE}/samples/subsys/usb/common/sample_usbd_init.c)
endif()

if(CONFIG_MINEW_ZMK_RADIO_TX)
  set(radio_root ${CMAKE_CURRENT_LIST_DIR}/../../radio)
  zephyr_include_directories(${radio_root}/src)
  zephyr_library_compile_definitions(RADIO_TX=1)
  zephyr_library_sources(${radio_root}/src/reliable.c ${radio_root}/src/secure_dtls.c
    ${radio_root}/src/secure_pair.c ${radio_root}/zmk/adapter.c)
endif()
