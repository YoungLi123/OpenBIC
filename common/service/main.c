#include "fru.h"
#include "hal_i2c.h"
#include "hal_wdt.h"
#include "ipmi.h"
#include "kcs.h"
#include "sensor.h"
#include "timer.h"
#include "usb.h"
#include <logging/log.h>
#include <drivers/i3c/i3c.h>
#include <device.h>

struct k_thread I3C_thread;
K_KERNEL_STACK_MEMBER(I3C_thread_stack, 4000);

int i3c_slave_mqueue_read(const struct device *dev, uint8_t *dest, int budget);
int i3c_slave_mqueue_write(const struct device *dev, uint8_t *src, int size);

void I3C_handler(void *arug0, void *arug1, void *arug2)
{
	const struct device *slave_mq;
	slave_mq = device_get_binding(DT_LABEL(DT_NODELABEL(i3c0_smq)));
	if (!slave_mq) {
		printk("slave-mq device not found\n");
		return;
	}

	uint8_t result[256];
	int ret, index;
	while (1) {
		ret = i3c_slave_mqueue_read(slave_mq, result, 256);
		if (ret) {
			printk("receive data from i3c bus: ");
			for (index = 0; index < ret; index++) {
				printk("0x%x ", result[index]);
			}
			printk("\n");
			ret = i3c_slave_mqueue_write(slave_mq, result, ret);
			if (ret)
				printk("send ibi error, %d\n", ret);
		}
		k_msleep(10);
	}
}

__weak void pal_pre_init()
{
	return;
}

__weak void pal_post_init()
{
	return;
}

__weak void pal_device_init()
{
	return;
}

__weak void pal_set_sys_status()
{
	return;
}

void main(void)
{
	printf("Hello, welcome to %s %s %x%x.%x.%x\n", PLATFORM_NAME, PROJECT_NAME, BIC_FW_YEAR_MSB,
	       BIC_FW_YEAR_LSB, BIC_FW_WEEK, BIC_FW_VER);

	wdt_init();
	util_init_timer();
	util_init_I2C();
	pal_pre_init();
	// 	sensor_init();
	// 	FRU_init();
	// 	ipmi_init();
	// #ifdef CONFIG_IPMI_KCS_ASPEED
	// 	kcs_init();
	// #endif
	// #ifdef CONFIG_USB
	// 	usb_dev_init();
	// #endif
	// 	pal_device_init();
	// 	pal_set_sys_status();
	// 	pal_post_init();

	k_thread_create(&I3C_thread, I3C_thread_stack, K_THREAD_STACK_SIZEOF(I3C_thread_stack),
			I3C_handler, NULL, NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0, K_NO_WAIT);
}
