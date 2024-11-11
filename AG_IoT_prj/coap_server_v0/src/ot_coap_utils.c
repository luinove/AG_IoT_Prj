#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
// #include <zephyr/net/net_pkt.h>
// #include <zephyr/net/net_l2.h>
#include <zephyr/net/openthread.h>
#include <net/coap_utils.h>
#include <openthread/coap.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/thread.h>
#include <coap_server_client_interface.h>

#include "ot_coap_utils.h"

// LOG_MODULE_REGISTER(ot_coap_utils, CONFIG_OT_COAP_UTILS_LOG_LEVEL);
LOG_MODULE_REGISTER(ot_coap_utils, LOG_LEVEL_ERR);
extern bool is_connected;

#define COAP_CLIENT_WORKQ_STACK_SIZE 2048
#define COAP_CLIENT_WORKQ_PRIORITY 5
K_THREAD_STACK_DEFINE(coap_client_workq_stack_area, COAP_CLIENT_WORKQ_STACK_SIZE);
static struct k_work_q coap_client_workq;

static struct k_work unicast_light_work;
static struct k_work multicast_light_work;
static struct k_work toggle_MTD_SED_work;
static struct k_work provisioning_work;

mtd_mode_toggle_cb_t on_mtd_mode_toggle;

extern uint16_t holding_reg[10];

static struct k_timer sed_timer;

/* Options supported by the server */
static const char *const light_option[] = { LIGHT_URI_PATH, NULL };
static const char *const provisioning_option[] = { PROVISIONING_URI_PATH,
						   NULL };

/* Thread multicast mesh local address */
static struct sockaddr_in6 multicast_local_addr = {
	.sin6_family = AF_INET6,
	.sin6_port = htons(COAP_PORT),
	.sin6_addr.s6_addr = { 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 },
	.sin6_scope_id = 0U
};

/* Variable for storing server address acquiring in provisioning handshake */
static char unique_local_addr_str[INET6_ADDRSTRLEN];
static struct sockaddr_in6 unique_local_addr = {
	.sin6_family = AF_INET6,
	.sin6_port = htons(COAP_PORT),
	.sin6_addr.s6_addr = {0, },
	.sin6_scope_id = 0U
};

struct server_context {
	struct otInstance *ot;
	bool provisioning_enabled;
	light_request_callback_t on_light_request;
	provisioning_request_callback_t on_provisioning_request;
};

static struct server_context srv_context = {
	.ot = NULL,
	.provisioning_enabled = false,
	.on_light_request = NULL,
	.on_provisioning_request = NULL,
};

/**@brief Definition of CoAP resources for provisioning. */
static otCoapResource provisioning_resource = {
	.mUriPath = PROVISIONING_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

/**@brief Definition of CoAP resources for light. */
static otCoapResource light_resource = {
	.mUriPath = LIGHT_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

static otError provisioning_response_send(otMessage *request_message,
					  const otMessageInfo *message_info)
{
	otError error = OT_ERROR_NO_BUFS;
	otMessage *response;
	const void *payload;
	uint16_t payload_size;

	response = otCoapNewMessage(srv_context.ot, NULL);
	if (response == NULL) {
		goto end;
	}

	otCoapMessageInit(response, OT_COAP_TYPE_NON_CONFIRMABLE,
			  OT_COAP_CODE_CONTENT);

	error = otCoapMessageSetToken(
		response, otCoapMessageGetToken(request_message),
		otCoapMessageGetTokenLength(request_message));
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapMessageSetPayloadMarker(response);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	payload = otThreadGetMeshLocalEid(srv_context.ot);
	payload_size = sizeof(otIp6Address);

	error = otMessageAppend(response, payload, payload_size);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapSendResponse(srv_context.ot, response, message_info);

	LOG_HEXDUMP_INF(payload, payload_size, "Sent provisioning response:");

end:
	if (error != OT_ERROR_NONE && response != NULL) {
		otMessageFree(response);
	}

	return error;
}

#define RESPONSE_POLL_PERIOD 10000

static bool is_mtd_in_med_mode(otInstance *instance)
{
	return otThreadGetLinkMode(instance).mRxOnWhenIdle;
}

static uint32_t poll_period;

static void poll_period_restore(void)
{
	otError error;
	otInstance *instance = openthread_get_default_instance();

	if (is_mtd_in_med_mode(instance)) {
		return;
	}

	if (poll_period) {
		error = otLinkSetPollPeriod(instance, poll_period);
		__ASSERT_NO_MSG(error == OT_ERROR_NONE);

		LOG_INF("Poll Period: %dms restored", poll_period);
		poll_period = 0;
	}
}

static int on_provisioning_reply(const struct coap_packet *response,
				 struct coap_reply *reply,
				 const struct sockaddr *from)
{
	int ret = 0;
	const uint8_t *payload;
	uint16_t payload_size = 0u;

	ARG_UNUSED(reply);
	ARG_UNUSED(from);

	payload = coap_packet_get_payload(response, &payload_size);

	if (payload == NULL ||
	    payload_size != sizeof(unique_local_addr.sin6_addr)) {
		LOG_ERR("Received data is invalid");
		ret = -EINVAL;
		goto exit;
	}

	memcpy(&unique_local_addr.sin6_addr, payload, payload_size);

	if (!inet_ntop(AF_INET6, payload, unique_local_addr_str,
		       INET6_ADDRSTRLEN)) {
		LOG_ERR("Received data is not IPv6 address: %d", errno);
		ret = -errno;
		goto exit;
	}

	LOG_INF("Received peer address: %s", unique_local_addr_str);

exit:
	if (IS_ENABLED(CONFIG_OPENTHREAD_MTD_SED)) {
		poll_period_restore();
	}

	return ret;
}

static void poll_period_response_set(void)
{
	otError error;

	otInstance *instance = openthread_get_default_instance();

	if (is_mtd_in_med_mode(instance)) {
		return;
	}

	if (!poll_period) {
		poll_period = otLinkGetPollPeriod(instance);

		error = otLinkSetPollPeriod(instance, RESPONSE_POLL_PERIOD);
		__ASSERT(error == OT_ERROR_NONE, "Failed to set pool period");

		LOG_INF("Poll Period: %dms set", RESPONSE_POLL_PERIOD);
	}
}

static void send_provisioning_request(struct k_work *item)
{
	ARG_UNUSED(item);
	
	if (IS_ENABLED(CONFIG_OPENTHREAD_MTD_SED)) {
		/* decrease the polling period for higher responsiveness */
		poll_period_response_set();
	}

	LOG_INF("Send 'provisioning' request");
	coap_send_request(COAP_METHOD_GET,
			  (const struct sockaddr *)&multicast_local_addr,
			  provisioning_option, NULL, 0u, on_provisioning_reply);
}

static void submit_work_if_connected(struct k_work *work)
{
	if (is_connected) {
		k_work_submit_to_queue(&coap_client_workq, work);
	} else {
		LOG_INF("Connection is broken");
	}
}

static void provisioning_request_handler(void *context, otMessage *message,
					 const otMessageInfo *message_info)
{
	otError error;
	otMessageInfo msg_info;

	ARG_UNUSED(context);

	if (!srv_context.provisioning_enabled) {
		LOG_WRN("Received provisioning request but provisioning "
			"is disabled");
		return;
	}

	LOG_INF("Received provisioning request");

	if ((otCoapMessageGetType(message) == OT_COAP_TYPE_NON_CONFIRMABLE) &&
	    (otCoapMessageGetCode(message) == OT_COAP_CODE_GET)) {
		msg_info = *message_info;
		memset(&msg_info.mSockAddr, 0, sizeof(msg_info.mSockAddr));

		error = provisioning_response_send(message, &msg_info);
		submit_work_if_connected(&provisioning_work);
		if (error == OT_ERROR_NONE) {
			srv_context.on_provisioning_request();
			srv_context.provisioning_enabled = false;
		}
	}
}

void configure_med_mode(void){
	LOG_INF("set med mode");
	otLinkModeConfig config;
	config.mRxOnWhenIdle = true;
	config.mDeviceType = false;
	config.mNetworkData = true;

	otThreadSetLinkMode(srv_context.ot, config);//set med mode
}

void configure_sed_mode(void){
	LOG_INF("set sed mode");
	otLinkModeConfig config;
	config.mRxOnWhenIdle = false;
	config.mDeviceType = false;
	config.mNetworkData = true;

	otThreadSetLinkMode(srv_context.ot, config);//set sed mode
	otLinkSetPollPeriod(srv_context.ot, RESPONSE_POLL_PERIOD);//set sed poll period
}

static bool con_reg;
static void sed_timer_handler(struct k_timer *timer_id){
	// configure_med_mode();
	LOG_INF("Wake up to perform data transmission");
	// srv_context.on_light_request(THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE);
	if(unique_local_addr.sin6_addr.s6_addr16[0] != 0){
		LOG_INF("unicast_light_work submit");
		k_work_submit_to_queue(&coap_client_workq, &unicast_light_work);
	}else{
		LOG_WRN("Peer address not set. Activate 'provisioning' option "
			"on the server side");
		return;	
	}
	// configure_sed_mode();
}



static void light_request_handler(void *context, otMessage *message,
				  const otMessageInfo *message_info)
{
	uint8_t command;

	ARG_UNUSED(context);

	if (otCoapMessageGetType(message) != OT_COAP_TYPE_NON_CONFIRMABLE) {
		LOG_ERR("Light handler - Unexpected type of message");
		goto end;
	}

	if (otCoapMessageGetCode(message) != OT_COAP_CODE_PUT) {
		LOG_ERR("Light handler - Unexpected CoAP code");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message), &command, 1) !=
	    1) {
		LOG_ERR("Light handler - Missing light command");
		goto end;
	}

	LOG_INF("Received light request: %c", command);

	srv_context.on_light_request(command);

	if(unique_local_addr.sin6_addr.s6_addr16[0] != 0){
		LOG_INF("unicast_light_work submit");
		// con_reg = false;
		// LOG_INF("get EID");
		// submit_work_if_connected(&provisioning_work);
		// con_reg = true;
		k_work_submit_to_queue(&coap_client_workq, &unicast_light_work);
		configure_sed_mode();
		k_timer_start(&sed_timer, K_SECONDS(10), K_SECONDS(10));
	}

end:
	return;
}

static void coap_default_handler(void *context, otMessage *message,
				 const otMessageInfo *message_info)
{
	ARG_UNUSED(context);
	ARG_UNUSED(message);
	ARG_UNUSED(message_info);

	LOG_INF("Received CoAP message that does not match any request "
		"or resource");
}

void ot_coap_activate_provisioning(void)
{
	srv_context.provisioning_enabled = true;
}

void ot_coap_deactivate_provisioning(void)
{
	srv_context.provisioning_enabled = false;
}

bool ot_coap_is_provisioning_active(void)
{
	return srv_context.provisioning_enabled;
}


static void toggle_one_light(struct k_work *item)
{
	if(unique_local_addr.sin6_addr.s6_addr16[0] != 0){
		srv_context.on_light_request(THREAD_COAP_UTILS_LIGHT_CMD_TOGGLE);
	}
	uint8_t payload = (uint8_t)holding_reg[0];
	LOG_INF("holding_reg[0]= %x", (uint8_t)holding_reg[0]);
	char reg_char[50];
	snprintk(reg_char,sizeof(reg_char),"%x,%x,%x,%x,%x,%x,%x,%x",holding_reg[0],holding_reg[1],holding_reg[2],holding_reg[3],holding_reg[4],holding_reg[5],holding_reg[6],holding_reg[7]);
	LOG_INF("reg_char: %s", reg_char);

	ARG_UNUSED(item);
	LOG_INF("unique_local_addr.sin6_addr.s6_addr16[0]= %d", unique_local_addr.sin6_addr.s6_addr16[0]);

	if (unique_local_addr.sin6_addr.s6_addr16[0] == 0) {
		LOG_WRN("Peer address not set. Activate 'provisioning' option "
			"on the server side");
		return;
	}

	LOG_INF("Send 'light' request to: %s", unique_local_addr_str);
	// coap_send_request(COAP_METHOD_PUT,
	// 		  (const struct sockaddr *)&unique_local_addr,
	// 		  light_option, &payload, sizeof(payload), NULL);

	coap_send_request(COAP_METHOD_PUT,
			  (const struct sockaddr *)&unique_local_addr,
			  light_option, &reg_char, sizeof(reg_char), NULL);
}


static void toggle_mesh_lights(struct k_work *item)
{
	static uint8_t command = (uint8_t)THREAD_COAP_UTILS_LIGHT_CMD_OFF;

	ARG_UNUSED(item);
	LOG_INF("mesh light");

	command = ((command == THREAD_COAP_UTILS_LIGHT_CMD_OFF) ?
			   THREAD_COAP_UTILS_LIGHT_CMD_ON :
			   THREAD_COAP_UTILS_LIGHT_CMD_OFF);

	LOG_INF("Send multicast mesh 'light' request");
	coap_send_request(COAP_METHOD_PUT,
			  (const struct sockaddr *)&multicast_local_addr,
			  light_option, &command, sizeof(command), NULL);
}


static void toggle_minimal_sleepy_end_device(struct k_work *item)
{
	otError error;
	otLinkModeConfig mode;
	struct openthread_context *context = openthread_get_default_context();

	__ASSERT_NO_MSG(context != NULL);

	openthread_api_mutex_lock(context);
	mode = otThreadGetLinkMode(context->instance);
	mode.mRxOnWhenIdle = !mode.mRxOnWhenIdle;
	error = otThreadSetLinkMode(context->instance, mode);
	openthread_api_mutex_unlock(context);

	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to set MLE link mode configuration");
	} else {
		on_mtd_mode_toggle(mode.mRxOnWhenIdle);
	}
}

static void update_device_state(void)
{
	struct otInstance *instance = openthread_get_default_instance();
	otLinkModeConfig mode = otThreadGetLinkMode(instance);
	on_mtd_mode_toggle(mode.mRxOnWhenIdle);
}

void coap_client_utils_init(mtd_mode_toggle_cb_t on_toggle)
{
	on_mtd_mode_toggle = on_toggle;
	LOG_INF("coap init");
	coap_init(AF_INET6, NULL);
	LOG_INF("coap client work quene init");
	k_work_queue_init(&coap_client_workq);
	k_work_queue_start(&coap_client_workq, coap_client_workq_stack_area,
					K_THREAD_STACK_SIZEOF(coap_client_workq_stack_area),
					COAP_CLIENT_WORKQ_PRIORITY, NULL);
	LOG_INF("add different work in coap client quene ");

	k_work_init(&unicast_light_work, toggle_one_light);
	k_work_init(&multicast_light_work, toggle_mesh_lights);
	k_work_init(&provisioning_work, send_provisioning_request);

	if (IS_ENABLED(CONFIG_OPENTHREAD_MTD_SED)) {
		k_work_init(&toggle_MTD_SED_work,
			    toggle_minimal_sleepy_end_device);
		update_device_state();
	}
	LOG_INF("end coap client utils init");
}

int ot_coap_init(provisioning_request_callback_t on_provisioning_request,
		 light_request_callback_t on_light_request)
{
	otError error;
	LOG_INF("SET server relevent parameters");
	srv_context.provisioning_enabled = false;
	srv_context.on_provisioning_request = on_provisioning_request;
	srv_context.on_light_request = on_light_request;

	srv_context.ot = openthread_get_default_instance();
	if (!srv_context.ot) {
		LOG_ERR("There is no valid OpenThread instance");
		error = OT_ERROR_FAILED;
		goto end;
	}
	// otIp6SetEnabled(srv_context.ot, true);
	// otThreadSetEnabled(srv_context.ot, true);
	// configure_sed_mode();

	LOG_INF("Initialize sed timer");
	k_timer_init(&sed_timer, sed_timer_handler, NULL);
	// configure_sed_mode();
	// k_timer_start(&sed_timer, K_SECONDS(10), K_SECONDS(10));

	LOG_INF("Intialize provisioning request handler");
	provisioning_resource.mContext = srv_context.ot;
	provisioning_resource.mHandler = provisioning_request_handler;

	LOG_INF("Initialize light request handler");
	light_resource.mContext = srv_context.ot;
	light_resource.mHandler = light_request_handler;

	LOG_INF("set default handler and add light resource and provisioning resource");
	otCoapSetDefaultHandler(srv_context.ot, coap_default_handler, NULL);
	otCoapAddResource(srv_context.ot, &light_resource);
	otCoapAddResource(srv_context.ot, &provisioning_resource);

	LOG_INF("start coap");
	error = otCoapStart(srv_context.ot, COAP_PORT);
	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to start OT CoAP. Error: %d", error);
		goto end;
	}

end:
	return error == OT_ERROR_NONE ? 0 : 1;
}
