/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __OT_COAP_UTILS_H__
#define __OT_COAP_UTILS_H__

#include <coap_server_client_interface.h>

/** @brief Type indicates function called when OpenThread connection
 *         is established.
 *
 * @param[in] item pointer to work item.
 */
// typedef void (*ot_connection_cb_t)(struct k_work *item);

/** @brief Type indicates function called when OpenThread connection is ended.
 *
 * @param[in] item pointer to work item.
 */
// typedef void (*ot_disconnection_cb_t)(struct k_work *item);

/** @brief Type indicates function called when the MTD modes are toggled.
 *
 * @param[in] val 1 if the MTD is in MED mode
 *                0 if the MTD is in SED mode
 */
typedef void (*mtd_mode_toggle_cb_t)(uint32_t val);

/** @brief Initialize CoAP client utilities.
 */
void coap_client_utils_init(mtd_mode_toggle_cb_t on_toggle);
// void coap_client_utils_init(ot_connection_cb_t on_connect,
// 			    ot_disconnection_cb_t on_disconnect,
// 			    mtd_mode_toggle_cb_t on_toggle);
/** @brief Toggle light on the CoAP server node.
 *
 * @note The CoAP server should be paired before to have an affect.
 */
void coap_client_toggle_one_light(void);

/** @brief Toggle lights on all CoAP servers in the network mesh.
 */
void coap_client_toggle_mesh_lights(void);

/** @brief Request for the CoAP server address to pair.
 *
 * @note Enable paring on the CoAP server to get the address.
 */
void coap_client_send_provisioning_request(void);

/** @brief Toggle SED to MED and MED to SED modes.
 *
 * @note Active when the device is working as Minimal Thread Device.
 */
void coap_client_toggle_minimal_sleepy_end_device(void);


/**@brief Type definition of the function used to handle light resource change.
 */
typedef void (*light_request_callback_t)(uint8_t cmd);
typedef void (*provisioning_request_callback_t)();

int ot_coap_init(provisioning_request_callback_t on_provisioning_request,
		 light_request_callback_t on_light_request);

void ot_coap_activate_provisioning(void);

void ot_coap_deactivate_provisioning(void);

bool ot_coap_is_provisioning_active(void);
#endif
