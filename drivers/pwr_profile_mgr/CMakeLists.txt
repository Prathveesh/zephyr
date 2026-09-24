# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_include_directories(.)
zephyr_library_sources(pwr_profile_mgr.c)
zephyr_library_sources_ifdef(CONFIG_SOC_SERIES_STM32F4X pwr_profile_mgr_stm32f4.c)
