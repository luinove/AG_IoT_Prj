#include <zephyr/kernel.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>
#include <ram_pwrdn.h>
#include <zephyr/device.h>
#include <openthread/message.h>
#include <zephyr/pm/device.h>

#include "coap_client_utils.h"
// #include "ot_coap_utils.h"

LOG_MODULE_REGISTER(coap_client, LOG_LEVEL_INF);
// LOG_MODULE_REGISTER(coap_client, CONFIG_COAP_CLIENT_LOG_LEVEL);

#define OT_CONNECTION_LED DK_LED1
#define LIGHT_LED DK_LED2
#define MTD_SED_LED DK_LED4

#define COAP_SERVER_WORKQ_STACK_SIZE 512
#define COAP_SERVER_WORKQ_PRIORITY 5

K_THREAD_STACK_DEFINE(coap_server_workq_stack_area, COAP_SERVER_WORKQ_STACK_SIZE);
static struct k_work_q coap_server_workq;

// static struct k_work provisioning_work;

static void on_light_request(uint8_t command)
{
	static uint8_t val;

	switch (command)
	{
	case THREAD_COAP_UTILS_LIGHT_CMD_ON:
		val = 1;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_OFF:
		dk_set_led_off(LIGHT_LED);
		val = 0;
		break;

	case THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE:
		val = !val;
		dk_set_led(LIGHT_LED, val);
		break;

	default:
		break;
	}
}

static void on_ot_connect(struct k_work *item)
{
	ARG_UNUSED(item);

	dk_set_led_on(OT_CONNECTION_LED);
}

static void on_ot_disconnect(struct k_work *item)
{
	ARG_UNUSED(item);

	dk_set_led_off(OT_CONNECTION_LED);
}

static void on_mtd_mode_toggle(uint32_t med)
{
#if IS_ENABLED(CONFIG_PM_DEVICE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cons))
	{
		return;
	}

	if (med)
	{
		pm_device_action_run(cons, PM_DEVICE_ACTION_RESUME);
	}
	else
	{
		pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	}
#endif
	dk_set_led(MTD_SED_LED, med);
}

static void on_button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t buttons = button_state & has_changed;

	if (buttons & DK_BTN1_MSK)
	{
		coap_client_toggle_one_light();
	}

	if (buttons & DK_BTN2_MSK)
	{
		coap_client_toggle_mesh_lights();
	}

	if (buttons & DK_BTN3_MSK)
	{
		coap_client_toggle_minimal_sleepy_end_device();
	}

	if (buttons & DK_BTN4_MSK)
	{
		coap_client_send_provisioning_request();
	}
}

int main(void)
{
	int ret;

	LOG_INF("Start CoAP-client sample");

	if (IS_ENABLED(CONFIG_RAM_POWER_DOWN_LIBRARY))
	{
		power_down_unused_ram();
	}

	ret = dk_buttons_init(on_button_changed);
	if (ret)
	{
		LOG_ERR("Cannot init buttons (error: %d)", ret);
		return 0;
	}

	ret = dk_leds_init();
	if (ret)
	{
		LOG_ERR("Cannot init leds, (error: %d)", ret);
		return 0;
	}

	k_work_queue_init(&coap_server_workq);
	k_work_queue_start(&coap_server_workq, coap_server_workq_stack_area,
					   K_THREAD_STACK_SIZEOF(coap_server_workq_stack_area),
					   COAP_SERVER_WORKQ_PRIORITY, NULL);
	// k_work_init(&provisioning_work, activate_provisioning);

	ret = ot_coap_init(&deactivate_provisionig, &on_light_request);
	if (ret)
	{
		LOG_ERR("Could not initialize OpenThread CoAP");
		goto end;
	}

	coap_client_utils_init(on_ot_connect, on_ot_disconnect, on_mtd_mode_toggle);
end:
	return 0;
}
