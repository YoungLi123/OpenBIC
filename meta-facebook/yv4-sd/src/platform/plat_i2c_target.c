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

/*
  NAME: I2C TARGET INIT
  FILE: plat_i2c_target.c
  DESCRIPTION: Provide i2c target EN/CFG table "I2C_TARGET_EN_TABLE[]/I2C_TARGET_CFG_TABLE[]" for init target config.
  AUTHOR: MouchenHung
  DATE/VERSION: 2021.11.26 - v1.1
  Note: 
    (1) "plat_i2c_target.h" is included by "hal_i2c_target.h"
*/

#include <zephyr.h>
#include <stdio.h>
#include <stdlib.h>
#include "plat_i2c_target.h"
#include "libutil.h"

/* LOG SET */
#include <logging/log.h>
#define LOG_LEVEL CONFIG_I2C_LOG_LEVEL
LOG_MODULE_REGISTER(plat_i2c_target);


static uint8_t test_data[16] = {0};
static uint8_t test20_command = 0;
static uint8_t fan_command = 0;


static bool test_read_requested(void *arg)
{
	CHECK_NULL_ARG_WITH_RETURN(arg, false);

	struct i2c_target_data *data = (struct i2c_target_data *)arg;

	int i;
	if (data->req_address == 0x20) {
		data->target_rd_msg.msg_length = 4;
		data->target_rd_msg.msg[0] = test20_command;
		data->target_rd_msg.msg[1] = test20_command + 1;
		data->target_rd_msg.msg[2] = test20_command + 2;
		data->target_rd_msg.msg[3] = test20_command + 3;
	} else if (data->req_address == 0x30) {
		data->target_rd_msg.msg_length = 16;
		for(i=0; i<16; i++) {
			data->target_rd_msg.msg[i] = test_data[i];
		}
	} else if (data->req_address == 0x50) {
		data->target_rd_msg.msg_length = 1;
		data->target_rd_msg.msg[0] = fan_command;
	}

	return false;
}

static void test_write_received(void *arg)
{
	CHECK_NULL_ARG(arg);

	struct i2c_target_data *data = (struct i2c_target_data *)arg;

	if (data->wr_buffer_idx == 1) {
		data->skip_msg_wr = true;
	}

	if (data->req_address == 0x30) {
		if (data->wr_buffer_idx < 17) {
			test_data[data->wr_buffer_idx - 1] = data->target_wr_msg.msg[data->wr_buffer_idx - 1];
		}
	} else if (data->req_address == 0x50) {
		if (data->wr_buffer_idx == 1) {
			fan_command = data->target_wr_msg.msg[data->wr_buffer_idx - 1] + 1;
		}
	}  else if (data->req_address == 0x20) {
		if (data->wr_buffer_idx == 1) {
			test20_command = data->target_wr_msg.msg[data->wr_buffer_idx - 1] - 1;
		}
	}
}

/* I2C target init-enable table */
const bool I2C_TARGET_ENABLE_TABLE[MAX_TARGET_NUM] = {
	TARGET_DISABLE, TARGET_DISABLE, TARGET_ENABLE,	TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
	TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE, TARGET_DISABLE,
};

/* I2C target init-config table */
const struct _i2c_target_config I2C_TARGET_CONFIG_TABLE[MAX_TARGET_NUM] = {
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0x40, 0xA, test_read_requested, test_write_received, 0x60, true, 0xA0, true }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
	{ 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA }, { 0xFF, 0xA },
};

