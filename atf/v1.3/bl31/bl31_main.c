/*
 * Copyright (c) 2013-2016, ARM Limited and Contributors. All rights reserved.
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

#include <arch.h>
#include <arch_helpers.h>
#include <assert.h>
#include <bl_common.h>
#include <bl31.h>
#include <context_mgmt.h>
#include <debug.h>
#include <platform.h>
#include <runtime_svc.h>
#include <string.h>
#include <mtk_plat_common.h>
#include <console.h>

/*******************************************************************************
 * This function pointer is used to initialise the BL32 image. It's initialized
 * by SPD calling bl31_register_bl32_init after setting up all things necessary
 * for SP execution. In cases where both SPD and SP are absent, or when SPD
 * finds it impossible to execute SP, this pointer is left as NULL
 ******************************************************************************/
static int32_t (*bl32_init)(void);

/*******************************************************************************
 * Variable to indicate whether next image to execute after BL31 is BL33
 * (non-secure & default) or BL32 (secure).
 ******************************************************************************/
static uint32_t next_image_type = NON_SECURE;

/*
 * Implement the ARM Standard Service function to get arguments for a
 * particular service.
 */
uintptr_t get_arm_std_svc_args(unsigned int svc_mask)
{
	/* Setup the arguments for PSCI Library */
	DEFINE_STATIC_PSCI_LIB_ARGS_V1(psci_args, bl31_warm_entrypoint);

	/* PSCI is the only ARM Standard Service implemented */
	assert(svc_mask == PSCI_FID_MASK);

	return (uintptr_t)&psci_args;
}

/*******************************************************************************
 * Simple function to initialise all BL31 helper libraries.
 ******************************************************************************/
void bl31_lib_init(void)
{
	cm_init();
}

/*******************************************************************************
 * BL31 is responsible for setting up the runtime services for the primary cpu
 * before passing control to the bootloader or an Operating System. This
 * function calls runtime_svc_init() which initializes all registered runtime
 * services. The run time services would setup enough context for the core to
 * swtich to the next exception level. When this function returns, the core will
 * switch to the programmed exception level via. an ERET.
 ******************************************************************************/
void bl31_main(void)
{
	NOTICE("BL31: %s\n", version_string);
	NOTICE("BL31: %s\n", build_message);
	unsigned long mpidr = read_mpidr();
	atf_arg_t_ptr teearg = (atf_arg_t_ptr)(uintptr_t)TEE_BOOT_INFO_ADDR;
	 
	if(teearg->atf_log_buf_size != 0)
	{
		//no log buffer do not care  , if you want to add log buffer , you need add MMU 
	    teearg->atf_aee_debug_buf_size = ATF_AEE_BUFFER_SIZE;
        teearg->atf_aee_debug_buf_start = teearg->atf_log_buf_start + teearg->atf_log_buf_size - ATF_AEE_BUFFER_SIZE;
		//mt_log_setup(teearg->atf_log_buf_start, teearg->atf_log_buf_size, teearg->atf_aee_debug_buf_size);
		tf_printf("ATF log service is registered (0x%x, aee:0x%x)\n", teearg->atf_log_buf_start, teearg->atf_aee_debug_buf_start);
	}
	else
	{
		teearg->atf_aee_debug_buf_size = 0;
		teearg->atf_aee_debug_buf_start = 0;        
	}    

	/* Perform platform setup in BL31 */
	bl31_platform_setup();

	/* Initialise helper libraries */
	bl31_lib_init();

	/* Initialize the runtime services e.g. psci. */
	INFO("BL31: Initializing runtime services\n");
	runtime_svc_init();
	dcsw_op_all(DCCSW);
    isb();

	/*
	 * All the cold boot actions on the primary cpu are done. We now need to
	 * decide which is the next image (BL32 or BL33) and how to execute it.
	 * If the SPD runtime service is present, it would want to pass control
	 * to BL32 first in S-EL1. In that case, SPD would have registered a
	 * function to intialize bl32 where it takes responsibility of entering
	 * S-EL1 and returning control back to bl31_main. Once this is done we
	 * can prepare entry into BL33 as normal.
	 */

	/*
	 * If SPD had registerd an init hook, invoke it.
	 */
#ifdef CC_BYPASS_BL32
	 tf_printf("[BL31] BYPASS secure OS for initialization!\n\r");
#else
    if(teearg->tee_support)
    {
        tf_printf("[BL31] Jump to secure OS for initialization!\n\r");
       
        if (bl32_init)
        {
            (*bl32_init)();
        }
        else
        {
            tf_printf("[ERROR] Secure OS is not initialized!\n\r");
            //assert(0);            
        }
	}
    else
    {
        tf_printf("[BL31] Jump to FIQD for initialization!\n\r");
        
        if (bl32_init)
        {
            (*bl32_init)();
        }
    }
#endif
    /*
     * Use the more complex exception vectors now that context
     * management is setup. SP_EL3 should point to a 'cpu_context'
     * structure which has an exception stack allocated.  The PSCI
     * service should have set the context.
     */
    assert(cm_get_context_by_index(platform_get_core_pos(mpidr),NON_SECURE));
    cm_set_next_eret_context(NON_SECURE);
    next_image_type = NON_SECURE;
	/*
	 * We are ready to enter the next EL. Prepare entry into the image
	 * corresponding to the desired security state after the next ERET.
	 */
	bl31_prepare_next_image_entry();

	/*
	 * Perform any platform specific runtime setup prior to cold boot exit
	 * from BL31
	 */
	bl31_plat_runtime_setup();
	//clear_uart_flag();
    tf_printf("[BL31] SHOULD not dump in UART also not in log buffer!\n\r");

}

/*******************************************************************************
 * Accessor functions to help runtime services decide which image should be
 * executed after BL31. This is BL33 or the non-secure bootloader image by
 * default but the Secure payload dispatcher could override this by requesting
 * an entry into BL32 (Secure payload) first. If it does so then it should use
 * the same API to program an entry into BL33 once BL32 initialisation is
 * complete.
 ******************************************************************************/
void bl31_set_next_image_type(uint32_t security_state)
{
	assert(sec_state_is_valid(security_state));
	next_image_type = security_state;
}

uint32_t bl31_get_next_image_type(void)
{
	return next_image_type;
}

/*******************************************************************************
 * This function programs EL3 registers and performs other setup to enable entry
 * into the next image after BL31 at the next ERET.
 ******************************************************************************/
void bl31_prepare_next_image_entry(void)
{
	entry_point_info_t *next_image_info;
	uint32_t image_type;

#if CTX_INCLUDE_AARCH32_REGS
	/*
	 * Ensure that the build flag to save AArch32 system registers in CPU
	 * context is not set for AArch64-only platforms.
	 */
	if (((read_id_aa64pfr0_el1() >> ID_AA64PFR0_EL1_SHIFT)
			& ID_AA64PFR0_ELX_MASK) == 0x1) {
		ERROR("EL1 supports AArch64-only. Please set build flag "
				"CTX_INCLUDE_AARCH32_REGS = 0");
		panic();
	}
#endif

	/* Determine which image to execute next */
	image_type = bl31_get_next_image_type();

	/* Program EL3 registers to enable entry into the next EL */
	next_image_info = bl31_plat_get_next_image_ep_info(image_type);
	assert(next_image_info);
	assert(image_type == GET_SECURITY_STATE(next_image_info->h.attr));

	INFO("BL31: Preparing for EL3 exit to %s world\n",
		(image_type == SECURE) ? "secure" : "normal");
	print_entry_point_info(next_image_info);
	cm_init_my_context(next_image_info);
	cm_prepare_el3_exit(image_type);
}

/*******************************************************************************
 * This function initializes the pointer to BL32 init function. This is expected
 * to be called by the SPD after it finishes all its initialization
 ******************************************************************************/
void bl31_register_bl32_init(int32_t (*func)(void))
{
	bl32_init = func;
}
