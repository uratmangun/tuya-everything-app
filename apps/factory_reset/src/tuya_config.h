/**
 * @file tuya_config.h
 * @brief Configuration for factory reset application
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __TUYA_CONFIG_H__
#define __TUYA_CONFIG_H__

/**
 * @brief Product ID from Tuya IoT Platform
 * You can get this from https://iot.tuya.com
 * Leave as placeholder - the reset will work without valid credentials
 */
#define TUYA_PRODUCT_ID    "xxxxxxxxxx"

/**
 * @brief UUID and AuthKey for device authorization
 * These are placeholders - factory reset doesn't require valid credentials
 */
#define TUYA_OPENSDK_UUID    "xxxxxxxxxxxxxxxxxxxxxxxxx"
#define TUYA_OPENSDK_AUTHKEY "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

#endif /* __TUYA_CONFIG_H__ */
