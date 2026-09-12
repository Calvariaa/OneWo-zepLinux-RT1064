.. _rt1064_shell_process:

zepLinux Shell for NXP i.MX RT1064
##################################

Overview
********

A shell environment with full process support, signal handling and SD card
commands, targeting the NXP i.MX RT1064 (Cortex-M7).

This sample is the RT1064 counterpart of ``rocket_pi_shell_process``. It is a
separate directory because that is how this repository isolates targets - see
``as32x601_shell_process`` and ``rocket_pi_shell_process``, which are likewise
near-identical copies adapted to their board. Keeping it separate means the
RocketPi sample stays byte-for-byte as upstream.

Features
********

- Process-based command execution (Embox-derived process model)
- Signal support (SIGINT, SIGTERM, ...) with per-process state
- Ctrl+C / Ctrl+D foreground process control
- Bytecode VM and ANL loader
- SD card commands:

  ===========  =========================================================
  ``sdinfo``   Card capacity, filesystem size, used and free space
  ``sdtest``   Raw sector write/read-back verification
  ``sdclear``  Fast clear: delete every entry (``format`` to re-create FAT)
  ``sdwipe``   Secure erase: overwrite the whole card with zeros
  ===========  =========================================================

- SD **hotplug**: inserting a card mounts it automatically, removing it
  unmounts. A background thread polls the card-detect line every 500 ms and
  reports ``[sd] card inserted - mounted at /SD:`` / ``[sd] card removed``.

Building and Running
********************

The repository carries its own Zephyr tree but no west workspace, and a build
also needs the compatibility shims described below, so the helper script is the
supported entry point:

.. code-block:: console

   ./build_rt1064.sh            # incremental
   ./build_rt1064.sh clean      # fresh configure

That produces ``build-rt1064/zephyr/zephyr.elf``, which can be flashed with any
runner for this part, e.g. over a DAPLink:

.. code-block:: console

   pyocd load --target mimxrt1064 -e chip build-rt1064/zephyr/zephyr.elf

Console is LPUART1 at 115200 8N1 (pads ``GPIO_AD_B0_12/13``).

Board support
*************

Everything board specific lives beside this file and is applied automatically by
Zephyr's ``boards/<board>.conf`` / ``boards/<board>.overlay`` convention, so
neither ``prj.conf`` nor the source tree has to know about any particular board:

``boards/mimxrt1064_evk.overlay``
   Two corrections against the NXP EVK, both for the MIMXRT1064-xVL core board:

   * **SD power** - the EVK switches the socket on ``GPIO1_19``, which is not
     connected here. This board uses an **AO3401 P-channel** MOSFET (high-side:
     source to 3V3, drain to the card, gate to the MCU) gated by pad ``B1_07``
     = ``GPIO2_IO23``. A P-channel switch conducts when its gate is pulled
     *low*, so the line is declared ``GPIO_ACTIVE_LOW``; the EVK's
     ``GPIO_ACTIVE_HIGH`` would hold the MOSFET off and the card would simply
     appear absent.
   * **Card detect** - the EVK detects on ``GPIO2_28``; this board wires CD to
     pad ``GPIO_SD_B1_04`` = ``GPIO3_IO04``, the SD1 group's dedicated
     ``USDHC1_CD_B``. ``cd-gpios`` alone only names the line, so a pinctrl group
     that muxes the pad is added and appended to ``pinctrl-0``.

``boards/mimxrt1064_evk.conf``
   Enables the block layer (``DISK_ACCESS``/``SDHC``), the ELM FAT filesystem,
   ``fs_mkfs`` and long file names.

   Long file names matter more than they look: with the default 8.3-only mode
   FatFS rejects any name longer than 8 characters with ``FR_INVALID_NAME``,
   which the filesystem layer surfaces as ``-ENOENT`` - so a plain
   ``mkdir /SD:/HELLOWORLD`` fails with a misleading "not found".
   ``FS_FATFS_LFN_MODE_HEAP`` is used rather than BSS because this sample runs
   each command in its own process and also has the hotplug thread; FatFS's BSS
   mode reuses one static buffer and is not thread-safe.

Portability fixes carried by this sample
****************************************

The following are inherited bugs that happen not to hurt the original STM32F401
target but break on the RT1064. They are fixed here rather than in
``rocket_pi_shell_process`` so that sample stays untouched:

* **Thread stack address and size** (``src/shell_process.c``).
  ``&stack_pool[i][0]`` is the *bottom* of the pool entry, and Cortex-M stacks
  grow down, so the region handed to ``k_thread_create`` grew straight into the
  MPU stack guard at the head of the pool. The size argument,
  ``K_THREAD_STACK_SIZEOF(TASK_STACK_SIZE)``, applied the macro to the *number*
  instead of a stack symbol and expanded to a negative value that became ~2^64
  as ``size_t``. Now ``K_THREAD_STACK_BUFFER(stack)`` and ``TASK_STACK_SIZE``.
* **Hard-coded SRAM window** (``src/commands.c``, ``src/debug.c``).
  Both sanity checks compared against ``0x20000000..0x20080000``, the STM32F401
  SRAM. RT1064 RAM is the SEMC SDRAM at ``0x80000000``, so every check failed
  and the sample reported "invalid process" for valid pointers. Replaced by
  ``sample_addr_in_ram()`` in ``src/debug.h``, driven by
  ``CONFIG_SRAM_BASE_ADDRESS``/``CONFIG_SRAM_SIZE``.
* **Nested signal handlers** (``src/commands.c``, ``cmd_loop`` and
  ``cmd_test_signal``). GNU C nested functions are reached through a trampoline
  GCC emits *on the stack*; with the stack in SDRAM and SDRAM mapped
  execute-never, calling one faults with an Instruction Access Violation. Both
  handlers were hoisted to file scope.
* The startup banner used to print a fixed board name; it now reports
  ``CONFIG_BOARD_TARGET`` and ``CONFIG_SOC``.

Sample output
*************

.. code-block:: console

   ========================================
     zepLinux Shell with Process Support
     board: mimxrt1064_evk/mimxrt1064   soc: mimxrt1064
     Based on Embox process model
   ========================================

   Init process PID: 1

   shell> sdinfo
   === SD card information ===
   sd: disk "SD" initialised
   disk name      : SD
   sector size    : 512 bytes
   sector count   : 7864320
   card capacity  : 3.75 GiB (4026531840 bytes)
   filesystem     : mounted at /SD:
     volume total : 3.74 GiB (4026007552 bytes)
     used         : 0 MiB (32768 bytes)
     free         : 3.74 GiB (4025974784 bytes)
     cluster size : 32768 bytes
   shell>
