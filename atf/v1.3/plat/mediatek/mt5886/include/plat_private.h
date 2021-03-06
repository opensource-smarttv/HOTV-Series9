/*
 * Copyright (c) 2016, ARM Limited and Contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __PLAT_PRIVATE_H__
#define __PLAT_PRIVATE_H__
#include <stdint.h>
#include <xlat_tables.h>
#include <fiq_smp_call.h>


/* Constants for accessing platform configuration */
#define CONFIG_GICD_ADDR		0
#define CONFIG_GICC_ADDR		1
#define CONFIG_GICH_ADDR		2
#define CONFIG_GICV_ADDR		3
#define CONFIG_MAX_AFF0		4
#define CONFIG_MAX_AFF1		5
/* Indicate whether the CPUECTLR SMP bit should be enabled. */
#define CONFIG_CPU_SETUP		6
#define CONFIG_BASE_MMAP		7
/* Indicates whether CCI should be enabled on the platform. */
#define CONFIG_HAS_CCI			8
#define CONFIG_HAS_TZC			9
#define CONFIG_LIMIT			10


void irq_raise_softirq(unsigned int map, unsigned int irq);
unsigned int get_ack_info();
void ack_sgi(unsigned int iar);
void mt_atf_trigger_irq();
void mask_wdt_fiq();
void gic_dist_save(void);
void gic_dist_restore(void);
//void gic_cpu_save(void);
//void gic_cpu_restore(void);
uint64_t mt_irq_dump_status(uint32_t irq);


void plat_configure_mmu_el3(unsigned long total_base,
					unsigned long total_size,
					unsigned long,
					unsigned long,
					unsigned long,
					unsigned long);

					
unsigned long mt_get_cfgvar(unsigned int);
int mt_config_setup(void);

void plat_cci_init(void);
void plat_cci_enable(void);
void plat_cci_disable(void);

/* Declarations for plat_mt_gic.c */
void plat_mt_gic_init(void);
void gic_setup(void);

/* Declarations for plat_topology.c */
int mt_setup_topology(void);
void plat_delay_timer_init(void);

void plat_mt_gic_driver_init(void);
void plat_mt_gic_init(void);
void plat_mt_gic_cpuif_enable(void);
void plat_mt_gic_cpuif_disable(void);
void plat_mt_gic_pcpu_init(void);

#endif /* __PLAT_PRIVATE_H__ */
