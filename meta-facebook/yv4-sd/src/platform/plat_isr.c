/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <logging/log.h>
#include <stdlib.h>
#include "libipmi.h"
#include "kcs.h"
#include "rg3mxxb12.h"
#include "power_status.h"
#include "sensor.h"
#include "snoop.h"
#include "apml.h"
#include "hal_gpio.h"
#include "hal_i2c.h"
#include "hal_i3c.h"
#include "util_sys.h"
#include "util_worker.h"
#include "plat_gpio.h"
#include "plat_class.h"
#include "plat_sensor_table.h"
#include "plat_i2c.h"
#include "plat_mctp.h"
#include "plat_apml.h"
#include "plat_i3c.h"
#include "plat_isr.h"
#include "plat_dimm.h"
#include "pcc.h"

#ifdef ENABLE_PLDM
#include "pldm_oem.h"
#endif

LOG_MODULE_REGISTER(plat_isr, LOG_LEVEL_DBG);

uint8_t hw_event_register[13] = { 0 };

/*
 *	TODO: I3C hub is supplied by DC power currently. When DC off, it can't be initialized.
 *  	  Therefore, BIC needs to implement a workaround to reinitialize the I3C hub during DC on.
 *        This part will be removed after the hardware design be modified.
 */
void reinit_i3c_hub()
{
	// i3c master initial
	I3C_MSG i3c_msg = { 0 };
	i3c_msg.bus = I3C_BUS_HUB;
	i3c_msg.target_addr = I3C_ADDR_HUB;

	const int rstdaa_count = 2;
	int ret = 0;

	for (int i = 0; i < rstdaa_count; i++) {
		ret = i3c_brocast_ccc(&i3c_msg, I3C_CCC_RSTDAA, I3C_BROADCAST_ADDR);
		if (ret != 0) {
			printf("Error to reset daa. count = %d\n", i);
		}
	}

	ret = i3c_brocast_ccc(&i3c_msg, I3C_CCC_SETAASA, I3C_BROADCAST_ADDR);
	if (ret != 0) {
		printf("Error to set daa\n");
	}

	i3c_attach(&i3c_msg);

	// Initialize I3C HUB
	if (!rg3mxxb12_i3c_mode_only_init(&i3c_msg, LDO_VOLT)) {
		printk("failed to initialize 1ou rg3mxxb12\n");
	}

	// Set FF/WF's EID
	send_cmd_to_dev_handler(NULL);
}

void switch_i3c_dimm_mux_to_cpu()
{
	switch_i3c_dimm_mux(I3C_MUX_CPU_TO_DIMM);
}

static void PROC_FAIL_handler()
{
	/* if have not received kcs and post code, add FRB3 event log. */
	if ((get_kcs_ok() == false) && (get_4byte_postcode_ok() == false)) {
		LOG_ERR("FRB3 event assert");
		if (PLDM_SUCCESS != send_event_log_to_bmc(FRB3_TIMER_EXPIRE, EVENT_ASSERTED)) {
			LOG_ERR("Failed to assert FRE3 event log.");
		};
	} else {
		/* Notification */
		LOG_INF("FRB3 checked pass");
	}
}

K_WORK_DELAYABLE_DEFINE(set_DC_on_5s_work, set_DC_on_delayed_status);
K_WORK_DEFINE(reinit_i3c_work, reinit_i3c_hub);
K_WORK_DEFINE(switch_i3c_dimm_work, switch_i3c_dimm_mux_to_cpu);
K_WORK_DELAYABLE_DEFINE(PROC_FAIL_work, PROC_FAIL_handler);

#define DC_ON_5_SECOND 5
#define PROC_FAIL_START_DELAY_SECOND 10
void ISR_DC_ON()
{
	set_DC_status(PWRGD_CPU_LVC3);

	bool dc_status = get_DC_status();

	if (dc_status) {
		k_work_schedule(&set_DC_on_5s_work, K_SECONDS(DC_ON_5_SECOND));
		k_work_submit(&reinit_i3c_work);
		k_work_submit(&switch_i3c_dimm_work);
		k_work_schedule(&PROC_FAIL_work, K_SECONDS(PROC_FAIL_START_DELAY_SECOND));
	} else {
		if (k_work_cancel_delayable(&PROC_FAIL_work) != 0) {
			LOG_ERR("Failed to cancel proc_fail delay work.");
		}
		reset_kcs_ok();
		reset_4byte_postcode_ok();

		set_DC_on_delayed_status();
	}
}

void ISR_POST_COMPLETE()
{
	set_post_status(FM_BIOS_POST_CMPLT_BIC_N);

	pal_check_sbrmi_command_code_length();

	if (get_post_status()) {
		set_tsi_threshold();
		disable_mailbox_completion_alert();
		enable_alert_signal();
		read_cpuid();
		//todo : add sel to bmc for assert
		hw_event_register[12]++;
	} else {
		if (get_DC_status()) { // Host is reset
			k_work_submit(&switch_i3c_dimm_work);
		}
	}
}

void ISR_BMC_READY()
{
	sync_bmc_ready_pin();
}

static void SLP3_handler()
{
	if ((gpio_get(FM_CPU_BIC_SLP_S3_N) == GPIO_HIGH) &&
	    (gpio_get(PWRGD_CPU_LVC3) == GPIO_LOW)) {
		//todo : add sel to bmc
		hw_event_register[0]++;
	}
}

K_WORK_DELAYABLE_DEFINE(SLP3_work, SLP3_handler);

#define DETECT_VR_WDT_DELAY_S 10
void ISR_SLP3()
{
	if (gpio_get(FM_CPU_BIC_SLP_S3_N) == GPIO_HIGH) {
		LOG_INF("slp3");
		k_work_schedule_for_queue(&plat_work_q, &SLP3_work,
					  K_SECONDS(DETECT_VR_WDT_DELAY_S));
	} else {
		if (k_work_cancel_delayable(&SLP3_work) != 0) {
			LOG_ERR("Failed to cancel delayable work.");
		}
	}
}

void ISR_DBP_PRSNT()
{
	if ((gpio_get(FM_DBP_PRESENT_N) == GPIO_HIGH)) {
		//todo : add sel to bmc for deassert
	} else {
		//todo : add sel to bmc for assert
		hw_event_register[1]++;
	}
}

void ISR_MB_THROTTLE()
{
	/* FAST_PROCHOT_N glitch workaround
	 * FAST_PROCHOT_N has a glitch and causes BIC to record MB_throttle deassertion SEL.
	 * Ignore this by checking whether MB_throttle is asserted before recording the deassertion.
	 */
	static bool is_mb_throttle_assert = false;
	if (gpio_get(RST_RSMRST_BMC_N) == GPIO_HIGH && get_DC_status()) {
		if ((gpio_get(FAST_PROCHOT_N) == GPIO_HIGH) && (is_mb_throttle_assert == true)) {
			//todo : add sel to bmc for deassert
			is_mb_throttle_assert = false;
		} else if ((gpio_get(FAST_PROCHOT_N) == GPIO_LOW) &&
			   (is_mb_throttle_assert == false)) {
			//todo : add sel to bmc for assert
			hw_event_register[2]++;
			is_mb_throttle_assert = true;
		} else {
			return;
		}
	}
}

void ISR_SOC_THMALTRIP()
{
	if (gpio_get(RST_CPU_RESET_BIC_N) == GPIO_HIGH) {
		//todo : add sel to bmc for assert
		hw_event_register[3]++;
	}
}

void ISR_SYS_THROTTLE()
{
	/* Same as MB_THROTTLE, glitch of FAST_PROCHOT_N will affect FM_CPU_BIC_PROCHOT_LVT3_N.
	 * Ignore the fake event by checking whether SYS_throttle is asserted before recording the deassertion.
	 */
	static bool is_sys_throttle_assert = false;
	if ((gpio_get(RST_CPU_RESET_BIC_N) == GPIO_HIGH) &&
	    (gpio_get(PWRGD_CPU_LVC3) == GPIO_HIGH)) {
		if ((gpio_get(FM_CPU_BIC_PROCHOT_LVT3_N) == GPIO_HIGH) &&
		    (is_sys_throttle_assert == true)) {
			//todo : add sel to bmc for deassert
			is_sys_throttle_assert = false;
		} else if ((gpio_get(FM_CPU_BIC_PROCHOT_LVT3_N) == GPIO_LOW) &&
			   (is_sys_throttle_assert == false)) {
			//todo : add sel to bmc for assert
			hw_event_register[4]++;
			is_sys_throttle_assert = true;
		} else {
			return;
		}
	}
}

void ISR_HSC_OC()
{
	if (gpio_get(RST_RSMRST_BMC_N) == GPIO_HIGH) {
		if (gpio_get(FM_HSC_TIMER_ALT_N) == GPIO_HIGH) {
			//todo : add sel to bmc for deassert
		} else {
			//todo : add sel to bmc for assert
			hw_event_register[5]++;
		}
	}
}

void ISR_PVDDCR_CPU0_OCP()
{
	if (get_DC_status() == true) {
		//todo : add sel to bmc for assert
		hw_event_register[7]++;
	}
}

void ISR_PVDDCR_CPU1_OCP()
{
	if (get_DC_status() == true) {
		//todo : add sel to bmc for assert
		hw_event_register[6]++;
	}
}

void ISR_PVDD11_S3_OCP()
{
	if (get_DC_status() == true) {
		//todo : add sel to bmc for assert
		hw_event_register[8]++;
	}
}

void ISR_UV_DETECT()
{
	if (gpio_get(RST_RSMRST_BMC_N) == GPIO_HIGH) {
		if (gpio_get(IRQ_UV_DETECT_N) == GPIO_HIGH) {
			//todo : add sel to bmc for deassert
		} else {
			//todo : add sel to bmc for assert
			hw_event_register[9]++;
		}
	}
}

void IST_PLTRST()
{
	//todo : add sel to bmc for assert
	hw_event_register[10]++;
}

void ISR_APML_ALERT()
{
	//todo : add sel to bmc for assert
	hw_event_register[11]++;
	LOG_INF("APML_ALERT detected");
	uint8_t status;
	if (apml_read_byte(apml_get_bus(), SB_RMI_ADDR, SBRMI_STATUS, &status))
		LOG_ERR("Failed to read RMI status.");

	if ((status & 0x02) && (apml_write_byte(apml_get_bus(), SB_RMI_ADDR, SBRMI_STATUS, 0x02)))
		LOG_ERR("Failed to clear SwAlertSts.");

	pldm_msg translated_msg = { 0 };
	uint8_t bmc_bus = I2C_BUS_BMC;
	uint8_t bmc_interface = pal_get_bmc_interface();

	if (bmc_interface == BMC_INTERFACE_I3C) {
		bmc_bus = I3C_BUS_BMC;
		translated_msg.ext_params.type = MCTP_MEDIUM_TYPE_TARGET_I3C;
		translated_msg.ext_params.i3c_ext_params.addr = I3C_STATIC_ADDR_BMC;
		translated_msg.ext_params.ep = MCTP_EID_BMC;
	} else {
		bmc_bus = I2C_BUS_BMC;
		translated_msg.ext_params.type = MCTP_MEDIUM_TYPE_SMBUS;
		translated_msg.ext_params.smbus_ext_params.addr = I2C_ADDR_BMC;
		translated_msg.ext_params.ep = MCTP_EID_BMC;
	}

	translated_msg.hdr.pldm_type = PLDM_TYPE_OEM;
	translated_msg.hdr.cmd = PLDM_OEM_WRITE_FILE_IO;
	translated_msg.hdr.rq = 1;

	struct pldm_oem_write_file_io_req *ptr = (struct pldm_oem_write_file_io_req *)malloc(
		sizeof(struct pldm_oem_write_file_io_req) + CPUID_SIZE);

	if (!ptr) {
		LOG_ERR("Failed to allocate memory.");
		return;
	}

	ptr->cmd_code = APML_ALERT;
	ptr->data_length = CPUID_SIZE;
	const uint8_t* cpuid_ptr = get_cpuid();
	memcpy(ptr->messages, cpuid_ptr, CPUID_SIZE);

	translated_msg.buf = (uint8_t *)ptr;
	translated_msg.len = sizeof(struct pldm_oem_write_file_io_req) + CPUID_SIZE;

	uint8_t resp_len = sizeof(struct pldm_oem_write_file_io_resp);
	uint8_t rbuf[resp_len];

	if (!mctp_pldm_read(find_mctp_by_bus(bmc_bus), &translated_msg, rbuf, resp_len)) {
		LOG_ERR("mctp_pldm_read fail");
		goto exit;
	}

	struct pldm_oem_write_file_io_resp *resp = (struct pldm_oem_write_file_io_resp *)rbuf;
	if (resp->completion_code != PLDM_SUCCESS) {
		LOG_ERR("Check reponse completion code fail %x", resp->completion_code);
		goto exit;
	}

exit:
	SAFE_FREE(ptr);

	return;
}