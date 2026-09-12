# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rt1064_shell_process)

target_sources(app PRIVATE
    src/main.c
    src/shell_process.c
    src/commands.c
    src/debug.c
    src/bytecode_vm.c
    src/anl_loader.c
    src/signal.c
    src/vfs_commands.c
)

# SD card shell commands. They need the ELM FAT filesystem (and through it
# <ff.h>), which boards/mimxrt1064_evk.conf enables for this sample's target.
if(CONFIG_FAT_FILESYSTEM_ELM)
  target_sources(app PRIVATE src/sd_commands.c)
endif()
