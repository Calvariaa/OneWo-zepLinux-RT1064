/*
 * Copyright (c) 2024 OneWo-rtLinux Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Debug utilities header
 */

#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdint.h>

#include <zephyr/kernel.h>

/**
 * @brief Whether an address lies inside the application's RAM region.
 *
 * The inherited code compared against the STM32F401 SRAM window
 * (0x20000000 .. 0x20080000) directly. That is only correct on that one part;
 * on the RT1064, whose RAM is the SEMC SDRAM at 0x80000000, both checks fail
 * unconditionally and the sample reports "invalid process" / "Invalid frame
 * pointer" for perfectly good pointers.
 *
 * Read the region from the configuration instead. CONFIG_SRAM_BASE_ADDRESS and
 * CONFIG_SRAM_SIZE follow the zephyr,sram chosen node, so this stays correct
 * whether RAM is internal SRAM, OCRAM or external SDRAM.
 *
 * @param p - address to test.
 * @returns true when p falls within [SRAM base, SRAM base + SRAM size).
 */
static inline bool sample_addr_in_ram(const void *p)
{
	uintptr_t addr = (uintptr_t)p;

	return addr >= (uintptr_t)CONFIG_SRAM_BASE_ADDRESS &&
	       addr < (uintptr_t)CONFIG_SRAM_BASE_ADDRESS +
		      (uintptr_t)CONFIG_SRAM_SIZE * 1024U;
}

/**
 * @brief Print current thread stack trace
 */
void print_stack_trace(void);

#endif /* DEBUG_H_ */
