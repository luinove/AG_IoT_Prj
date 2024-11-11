/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/pm/device.h>

#include "ot_coap_utils.h"

// LOG_MODULE_REGISTER(coap_server, CONFIG_COAP_SERVER_LOG_LEVEL);
LOG_MODULE_REGISTER(coap_server, LOG_LEVEL_INF);

#define MODBUS_NODE 		DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

#define OT_CONNECTION_LED 	DK_LED1
#define MTD_SED_LED 		DK_LED2
#define PROVISIONING_LED 	DK_LED3
#define LIGHT_LED 			DK_LED4

#define COAP_SERVER_WORKQ_STACK_SIZE 512
#define COAP_SERVER_WORKQ_PRIORITY 5

bool is_connected;

K_THREAD_STACK_DEFINE(coap_server_workq_stack_area, COAP_SERVER_WORKQ_STACK_SIZE);
static struct k_work_q coap_server_workq;
static struct k_work provisioning_work;
static struct k_timer led_timer;
static struct k_timer provisioning_timer;

/* modbus */
int client_iface;
struct modbus_iface_param client_param = {
	.mode = MODBUS_MODE_RTU,
	.rx_timeout = 50000,
	.serial = {
		.baud = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits_client = UART_CFG_STOP_BITS_1,
	},
};
uint16_t holding_reg[10] = {0};

int init_modbus_client(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_NODE)};
	client_iface = modbus_iface_get_by_name(iface_name);
	return modbus_init_client(client_iface, client_param);
}
uint16_t modbus_uid_SED 	= 0x01;
uint16_t reg_adr_SED[7] 	= {0x06, 0x07, 0x08, 0x09, 0x1E, 0x1F, 0x20};
uint16_t reg_qty_SED 		= 1;

void read_sensor_data(void){
	uint8_t err;
	for (int i=0; i<7; i++){
		LOG_INF("%d",i);
		err = modbus_read_holding_regs(client_iface,modbus_uid_SED,reg_adr_SED[i],holding_reg,reg_qty_SED);
		if (err != 0) {
			LOG_ERR("FC03 failed with %d", err);
			return;
		}
		holding_reg[i+1] = holding_reg[0];
		LOG_INF("%d: %x;\n",reg_adr_SED[i],holding_reg[i+1]);
	}
	holding_reg[0] = modbus_uid_SED;
}

static void on_light_request(uint8_t command)
{
	static uint8_t val;

	switch (command) {
	case THREAD_COAP_UTILS_LIGHT_CMD_ON:
		dk_set_led_on(LIGHT_LED);
		val = 1;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_OFF:
		dk_set_led_off(LIGHT_LED);
		val = 0;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE:
		val = !val;
		// dk_set_led(LIGHT_LED, val);
		read_sensor_data();
		break;

	default:
		break;
	}
}

static void activate_provisioning(struct k_work *item)
{
	ARG_UNUSED(item);
	LOG_INF("SET srv_context.provisioning_enabled = true");
	ot_coap_activate_provisioning();
	LOG_INF("start LED TIMER and PROVISIONING TIMER");
	k_timer_start(&led_timer, K_MSEC(100), K_MSEC(100));
	k_timer_start(&provisioning_timer, K_SECONDS(5), K_NO_WAIT);

	LOG_INF("Provisioning activated");
}

static void deactivate_provisionig(void)
{
	LOG_INF("STOP LED TIMER and PROVISIONING TIMER");
	k_timer_stop(&led_timer);
	k_timer_stop(&provisioning_timer);

	LOG_INF("if srv_context.provisioning_enabled = true, change it to false");
	if (ot_coap_is_provisioning_active()) {
		ot_coap_deactivate_provisioning();
		LOG_INF("Provisioning deactivated");
	}
}

static void on_provisioning_timer_expiry(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);

	deactivate_provisionig();
}

static void on_led_timer_expiry(struct k_timer *timer_id)
{
	static uint8_t val = 1;

	ARG_UNUSED(timer_id);
	dk_set_led(PROVISIONING_LED, val);
	val = !val;
}

static void on_led_timer_stop(struct k_timer *timer_id)
{
	ARG_UNUSED(timer_id);

	dk_set_led_off(PROVISIONING_LED);
}

static void on_mtd_mode_toggle(uint32_t med)
{
#if IS_ENABLED(CONFIG_PM_DEVICE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cons)) {
		return;
	}

	if (med) {
		pm_device_action_run(cons, PM_DEVICE_ACTION_RESUME);
	} else {
		pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
	dk_set_led(MTD_SED_LED, med);
}

static void on_button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t buttons = button_state & has_changed;
	LOG_INF("if BUTTON4 state changed, submitted provisioning work to coap server work quene");
	if (buttons & DK_BTN4_MSK) {
		k_work_submit_to_queue(&coap_server_workq, &provisioning_work);
	}
}

static void on_thread_state_changed(otChangedFlags flags, struct openthread_context *ot_context,
				    void *user_data)
{
	if (flags & OT_CHANGED_THREAD_ROLE) {
		LOG_INF("OT Thread Role : %d", otThreadGetDeviceRole(ot_context->instance));
		switch (otThreadGetDeviceRole(ot_context->instance)) {
		case OT_DEVICE_ROLE_CHILD:
		case OT_DEVICE_ROLE_ROUTER:
		case OT_DEVICE_ROLE_LEADER:
			dk_set_led_on(OT_CONNECTION_LED);
			is_connected = true;
			break;

		case OT_DEVICE_ROLE_DISABLED:
		case OT_DEVICE_ROLE_DETACHED:
		default:
			dk_set_led_off(OT_CONNECTION_LED);
			is_connected = false;
			deactivate_provisionig();
			break;
		}
	}
}
static struct openthread_state_changed_cb ot_state_chaged_cb = { .state_changed_cb =
									 on_thread_state_changed };

int main(void)
{
	int ret;

	LOG_INF("Initialize LEDs");
	ret = dk_leds_init();
	if (ret) {
		LOG_ERR("Could not initialize leds, err code: %d", ret);
		goto end;
	}
	
	LOG_INF("Initialize modbus client");
	if (init_modbus_client()) {
		LOG_ERR("Modbus RTU client initialization failed");
		return 0;
	}
	k_msleep(1000);

	LOG_INF("Start CoAP-server sample");
	LOG_INF("led timer: PROVISIONING LED Blinking");
	LOG_INF("provisioning timer: ");
	k_timer_init(&led_timer, on_led_timer_expiry, on_led_timer_stop);
	k_timer_init(&provisioning_timer, on_provisioning_timer_expiry, NULL);
	LOG_INF("Initialize work quene coap server work");
	k_work_queue_init(&coap_server_workq);
	LOG_INF("start work quene coap server work");
	k_work_queue_start(&coap_server_workq, coap_server_workq_stack_area,
					K_THREAD_STACK_SIZEOF(coap_server_workq_stack_area),
					COAP_SERVER_WORKQ_PRIORITY, NULL);
	LOG_INF("Initialize activate provisioning function to procisioning work");
	k_work_init(&provisioning_work, activate_provisioning);

	LOG_INF("Initialize BUTTONs");
	ret = dk_buttons_init(on_button_changed);
	if (ret) {
		LOG_ERR("Cannot init buttons (error: %d)", ret);
		goto end;
	}

	LOG_INF("start ot coap init function");
	ret = ot_coap_init(&deactivate_provisionig, &on_light_request);
	if (ret) {
		LOG_ERR("Could not initialize OpenThread CoAP");
		goto end;
	}
	coap_client_utils_init(on_mtd_mode_toggle);
	LOG_INF("1");
	openthread_state_changed_cb_register(openthread_get_default_context(), &ot_state_chaged_cb);
	LOG_INF("2");
	openthread_start(openthread_get_default_context());
	LOG_INF("3");

end:
	return 0;
}
