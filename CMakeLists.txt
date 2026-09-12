if(CONFIG_ZMK_FEATURE_BATTERY_MONITOR)
  zephyr_library()
  zephyr_library_sources(src/battery_monitor.c)
endif()
