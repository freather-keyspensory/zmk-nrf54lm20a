zephyr_include_directories(include)
zephyr_syscall_header(${CMAKE_CURRENT_LIST_DIR}/include/zephyr/drivers/kscan.h)
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_MINEW_MANUAL_KEY manual_key.c)
