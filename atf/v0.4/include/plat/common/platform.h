/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
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

#ifndef __PLATFORM_H__
#define __PLATFORM_H__

#include <stdint.h>
#include <fiq_smp_call.h>


/*******************************************************************************
 * Forward declarations
 ******************************************************************************/
struct plat_pm_ops;
struct meminfo;
struct image_info;
struct entry_point_info;
struct bl31_params;

/*******************************************************************************
 * Function declarations
 ******************************************************************************/
/*******************************************************************************
 * Mandatory common functions
 ******************************************************************************/
uint64_t plat_get_syscnt_freq(void);
int plat_get_image_source(const char *image_name,
			uintptr_t *dev_handle,
			uintptr_t *image_spec);
unsigned long plat_get_ns_image_entrypoint(void);

/*******************************************************************************
 * Mandatory interrupt management functions
 ******************************************************************************/
uint32_t plat_ic_get_pending_interrupt_id(void);
uint32_t plat_ic_get_pending_interrupt_type(void);
uint32_t plat_ic_acknowledge_interrupt(void);
uint32_t plat_ic_get_interrupt_type(uint32_t id);
void plat_ic_end_of_interrupt(uint32_t id);
uint32_t plat_interrupt_type_to_line(uint32_t type,
				     uint32_t security_state);
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
/*******************************************************************************
 * Optional common functions (may be overridden)
 ******************************************************************************/
unsigned int platform_get_core_pos(unsigned long mpidr);
unsigned long platform_get_stack(unsigned long mpidr);
void plat_report_exception(unsigned long);

/*******************************************************************************
 * Mandatory BL1 functions
 ******************************************************************************/
void bl1_plat_arch_setup(void);
void bl1_platform_setup(void);
struct meminfo *bl1_plat_sec_mem_layout(void);

/*
 * This function allows the platform to change the entrypoint information for
 * BL2, after BL1 has loaded BL2 into memory but before BL2 is executed.
 */
void bl1_plat_set_bl2_ep_info(struct image_info *image,
			      struct entry_point_info *ep);

/*******************************************************************************
 * Optional BL1 functions (may be overridden)
 ******************************************************************************/
void init_bl2_mem_layout(struct meminfo *,
			struct meminfo *,
			unsigned int,
			unsigned long);

/*******************************************************************************
 * Mandatory BL2 functions
 ******************************************************************************/
void bl2_plat_arch_setup(void);
void bl2_platform_setup(void);
struct meminfo *bl2_plat_sec_mem_layout(void);

/*
 * This function returns a pointer to the shared memory that the platform has
 * kept aside to pass trusted firmware related information that BL3-1
 * could need
 */
struct bl31_params *bl2_plat_get_bl31_params(void);

/*
 * This function returns a pointer to the shared memory that the platform
 * has kept to point to entry point information of BL31 to BL2
 */
struct entry_point_info *bl2_plat_get_bl31_ep_info(void);

/*
 * This function flushes to main memory all the params that are
 * passed to BL3-1
 */
void bl2_plat_flush_bl31_params(void);

/*
 * The next 3 functions allow the platform to change the entrypoint
 * information for the 3rd level BL images, after BL2 has loaded the 3rd
 * level BL images into memory but before BL3-1 is executed.
 */
void bl2_plat_set_bl31_ep_info(struct image_info *image,
			       struct entry_point_info *ep);

void bl2_plat_set_bl32_ep_info(struct image_info *image,
			       struct entry_point_info *ep);

void bl2_plat_set_bl33_ep_info(struct image_info *image,
			       struct entry_point_info *ep);

/* Gets the memory layout for BL32 */
void bl2_plat_get_bl32_meminfo(struct meminfo *mem_info);

/* Gets the memory layout for BL33 */
void bl2_plat_get_bl33_meminfo(struct meminfo *mem_info);

/*******************************************************************************
 * Optional BL2 functions (may be overridden)
 ******************************************************************************/

/*******************************************************************************
 * Mandatory BL3-1 functions
 ******************************************************************************/
void bl31_early_platform_setup(struct bl31_params *from_bl2,
				void *plat_params_from_bl2);
void bl31_plat_arch_setup(void);
void bl31_platform_setup(void);
struct entry_point_info *bl31_plat_get_next_image_ep_info(uint32_t type);

/*******************************************************************************
 * Mandatory PSCI functions (BL3-1)
 ******************************************************************************/
int platform_setup_pm(const struct plat_pm_ops **);
int plat_get_max_afflvl(void);
unsigned int plat_get_aff_count(unsigned int, unsigned long);
unsigned int plat_get_aff_state(unsigned int, unsigned long);

/*******************************************************************************
 * Optional BL3-1 functions (may be overridden)
 ******************************************************************************/
void bl31_plat_enable_mmu();

/*******************************************************************************
 * Mandatory BL3-2 functions (only if platform contains a BL3-2)
 ******************************************************************************/
void bl32_platform_setup(void);

/*******************************************************************************
 * Optional BL3-2 functions (may be overridden)
 ******************************************************************************/
void bl32_plat_enable_mmu();

/*******************************************************************************
 * MTK struct
 ******************************************************************************/
typedef enum {
    BOOT_OPT_64S3 = 0,
    BOOT_OPT_64S1,
    BOOT_OPT_32S3,
    BOOT_OPT_32S1,
    BOOT_OPT_64N2,
    BOOT_OPT_64N1,
    BOOT_OPT_32N2,
    BOOT_OPT_32N1,
    BOOT_OPT_UNKNOWN
} boot_option_t;

#define DEVINFO_SIZE 4
#define ATF_AEE_BUFFER_SIZE             (0x4000)//16KB
typedef struct {
    unsigned int atf_magic;
    unsigned int tee_support;
    unsigned int tee_entry;
    unsigned int tee_boot_arg_addr;
    unsigned int hwuid[4];     // HW Unique id for t-base used  
    unsigned int HRID[2];      // HW random id for t-base used  
    unsigned int atf_log_port;
    unsigned int atf_log_baudrate;
    unsigned int atf_log_buf_start;
    unsigned int atf_log_buf_size;
    unsigned int atf_irq_num;
    unsigned int devinfo[DEVINFO_SIZE];
    unsigned int atf_aee_debug_buf_start;
    unsigned int atf_aee_debug_buf_size;
} atf_arg_t, *atf_arg_t_ptr;


extern unsigned int BL33_ARG0;
extern unsigned int BL33_ARG1;
extern unsigned int BL33_ARG2;
extern unsigned int BL33_START_ADDRESS;
extern unsigned int TEE_BOOT_INFO_ADDR;


struct atf_aee_regs {
    uint64_t    regs[31];
    uint64_t    sp;
    uint64_t    pc;
    uint64_t    pstate;
};

/* WDT callback function */
void aee_wdt_dump();
extern uint64_t wdt_kernel_cb_addr;

#endif /* __PLATFORM_H__ */
