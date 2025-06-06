cmake_minimum_required(VERSION 3.20.0)

set(BOARD rp2040_zero)
find_package(Zephyr)

project(tidbit-gateron)

target_sources(app PRIVATE
  src/main.c
  src/sample_usbd_init.c
)
