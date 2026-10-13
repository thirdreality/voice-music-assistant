/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <cjson/cJSON.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"
#include "monitor/bt.h"

#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/hci.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"

#ifdef __linux__
#include <net/if.h>
#endif

static struct server *server_create(int fd);

static void start_advertising(void);
static void stop_advertising(void);
static void update_advertising_data(void);
static void set_adv_data(void);
static void reset_no_client_timeout(void);
static void no_client_timeout_cb(int timeout_id, void *user_data);
static const char *get_device_name(void);

// Improv BLE UUIDs
#define IMPROV_SERVICE_UUID_STR "00467768-6228-2272-4663-277478268000"
#define IMPROV_CHAR_STATE_UUID_STR "00467768-6228-2272-4663-277478268001"
#define IMPROV_CHAR_ERROR_UUID_STR "00467768-6228-2272-4663-277478268002"
#define IMPROV_CHAR_RPC_UUID_STR "00467768-6228-2272-4663-277478268003"
#define IMPROV_CHAR_RESULT_UUID_STR "00467768-6228-2272-4663-277478268004"
#define IMPROV_CHAR_CAPS_UUID_STR "00467768-6228-2272-4663-277478268005"

#define MUSE_SERVICE_UUID_STR "7fdd3d1c-38ea-46cf-8b46-314ecf5f240c"
#define MUSE_RX_UUID_STR "4d593029-28a2-4a6e-a1f0-3c2d5e8f9b01"
#define MUSE_TX_UUID_STR "d75dc4ca-7b2b-4e9c-8f0a-1d2e3f4a5b6c"
#define MUSE_CONTROL_SOCKET "/tmp/muse-gadget-control.sock"
#define MUSE_CHUNK_MAGIC 0xFE
#define MUSE_MAX_MESSAGE 8192

#define ATT_CID 4

static struct hci_dev_info hdi;
static int ctl;
static struct server *server;
static bool wifi_config_completed = false;

// Timeout settings (15 minutes = 900 seconds)
#define NO_CLIENT_TIMEOUT_SECONDS 900

struct server {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;

	bool state_notify_enabled;
	bool error_notify_enabled;
	bool result_notify_enabled;
	bool muse_notify_enabled;
	pthread_mutex_t notification_lock;

	uint8_t muse_buffer[MUSE_MAX_MESSAGE];
	size_t muse_buffer_len;
	uint8_t muse_chunk_count;
	uint8_t muse_next_chunk;

	// BLE GATT long write buffer for RPC
#define MAX_WRITE_BUFFER 1024
	uint8_t write_buffer[MAX_WRITE_BUFFER];
	size_t write_buffer_len;
	bool write_in_progress;
};

static struct {
	struct gatt_db_attribute *cap;
	struct gatt_db_attribute *state;
	struct gatt_db_attribute *error;
	struct gatt_db_attribute *rpc;
	struct gatt_db_attribute *result;
	uint16_t result_handle;
	/* Value handles from gatt_db_attribute_get_handle() - used for notifications */
	uint16_t state_value_handle;
	uint16_t error_value_handle;
} improv_chars;

static struct {
	struct gatt_db_attribute *rx;
	struct gatt_db_attribute *tx;
	uint16_t tx_value_handle;
} muse_chars;

// Global state management variables
static bool client_connected = false;
static bool advertising = false;
static volatile bool should_exit = false;
static unsigned int no_client_timeout_id = 0;
static unsigned int adv_rotate_timeout_id = 0;

// ESPHome-style advertising cadence: name for 1s every 60s
#define NAME_ADVERTISING_INTERVAL_MS 60000
#define NAME_ADVERTISING_DURATION_MS 1000
static uint64_t last_name_adv_time_ms = 0;
static bool adv_name_active = false;

// Improv state
static uint8_t improv_state = 0x02; // Authorized (no physical auth required)
static uint8_t improv_error = 0x00; // No error
static uint8_t improv_caps = 0x01; // Identify only (match ESPHome default)

static void delayed_exit_cb(int timeout_id, void *user_data)
{
	(void)timeout_id;
	(void)user_data;
	should_exit = true;
	mainloop_quit();
}

static void delayed_session_reset_cb(int timeout_id, void *user_data)
{
	(void)timeout_id;
	(void)user_data;
	mainloop_quit();
}

static void reset_improv_session_state(void)
{
	if (wifi_config_completed)
		return;

	improv_state = 0x02; /* Authorized */
	improv_error = 0x00; /* No error */
}

static int run_wifi_connect_command(const char *ssid, const char *password)
{
	const char *script = "/usr/share/thirdreality/script/wifi_connect";
	const char *safe_password = password ? password : "";
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;

	if (pid == 0) {
		execl(script, script, "connect", ssid, safe_password, (char *)NULL);
		_exit(127);
	}

	do {
		if (waitpid(pid, &status, 0) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		break;
	} while (true);

	if (!WIFEXITED(status))
		return -1;

	return WEXITSTATUS(status);
}

static void format_wifi_error_response(int exit_code, char *response, size_t response_len)
{
	const char *err_msg;

	switch (exit_code) {
	case 10:
		err_msg = "SSID not found";
		break;
	case 12:
		err_msg = "Failed to get IP";
		break;
	case 13:
		err_msg = "Connect failed";
		break;
	case 14:
		err_msg = "Switch WiFi failed";
		break;
	case 127:
		err_msg = "wifi_connect exec failed";
		break;
	default:
		err_msg = "Unknown error";
		break;
	}

	snprintf(response, response_len, "{\"err\":\"%s\"}", err_msg);
}

static int read_wifi_ip_response(char *response, size_t response_len)
{
	FILE *device_info_fp = fopen("/data/conf/device.json", "r");
	if (!device_info_fp) {
		snprintf(response, response_len, "{\"err\":\"Failed to read IP\"}");
		return -1;
	}

	fseek(device_info_fp, 0, SEEK_END);
	long file_size = ftell(device_info_fp);
	fseek(device_info_fp, 0, SEEK_SET);

	char *device_info_content = malloc(file_size + 1);
	if (!device_info_content) {
		fclose(device_info_fp);
		snprintf(response, response_len, "{\"err\":\"Memory error\"}");
		return -1;
	}

	fread(device_info_content, 1, file_size, device_info_fp);
	device_info_content[file_size] = '\0';
	fclose(device_info_fp);

	cJSON *device_info = cJSON_Parse(device_info_content);
	free(device_info_content);

	if (!device_info) {
		snprintf(response, response_len, "{\"err\":\"Invalid device_info\"}");
		return -1;
	}

	cJSON *network = cJSON_GetObjectItem(device_info, "network");
	cJSON *ip_item = network ? cJSON_GetObjectItem(network, "ip") : NULL;

	if (!ip_item || !cJSON_IsString(ip_item)) {
		snprintf(response, response_len, "{\"err\":\"IP not found\"}");
		cJSON_Delete(device_info);
		return -1;
	}

	char *ip = ip_item->valuestring;
	if (!ip || strlen(ip) == 0) {
		snprintf(response, response_len, "{\"err\":\"IP is empty\"}");
		cJSON_Delete(device_info);
		return -1;
	}

	snprintf(response, response_len, "{\"ip\":\"%s\"}", ip);

	cJSON_Delete(device_info);
	return 0;
}

static int process_wifi_config(const char *json_str, char *response, size_t response_len)
{
	cJSON *root = cJSON_Parse(json_str);
	if (!root) {
		snprintf(response, response_len, "{\"err\":\"bad fmt\"}");
		return -1;
	}
	cJSON *ssid_item = cJSON_GetObjectItem(root, "ssid");
	if (!ssid_item || !cJSON_IsString(ssid_item)) {
		cJSON_Delete(root);
		snprintf(response, response_len, "{\"err\":\"bad ssid\"}");
		return -1;
	}

	char *ssid = ssid_item->valuestring;
	cJSON *password_item = cJSON_GetObjectItem(root, "pw");
	const char *password = "";
	if (password_item && cJSON_IsString(password_item)) {
		password = password_item->valuestring;
	}

	int exit_code = run_wifi_connect_command(ssid, password);

	if (exit_code != 0) {
		format_wifi_error_response(exit_code, response, response_len);
		cJSON_Delete(root);
		return -1;
	}

	if (read_wifi_ip_response(response, response_len) != 0) {
		cJSON_Delete(root);
		return -1;
	}
	cJSON_Delete(root);
	return 0;
}

static int process_wifi_config_improv(const char *ssid, const char *pw, char *out_ip, size_t out_ip_len)
{
	char response[256];
	int exit_code = run_wifi_connect_command(ssid, pw);

	if (exit_code != 0) {
		format_wifi_error_response(exit_code, response, sizeof(response));
		return -1;
	}

	if (read_wifi_ip_response(response, sizeof(response)) != 0) {
		return -1;
	}

	cJSON *root = cJSON_Parse(response);
	if (!root)
		return -1;
	cJSON *ip_item = cJSON_GetObjectItem(root, "ip");
	if (!ip_item || !cJSON_IsString(ip_item)) {
		cJSON_Delete(root);
		return -1;
	}

	snprintf(out_ip, out_ip_len, "%s", ip_item->valuestring);
	cJSON_Delete(root);
	return 0;
}

static void att_disconnect_cb(int err, void *user_data)
{
	struct server *server = user_data;
	(void)server;
	(void)err;

	printf("[BLE] Client disconnected (err=%d)\n", err);
	client_connected = false;

	/* Reset state so the next BLE session starts clean. */
	reset_improv_session_state();

	if (wifi_config_completed) {
		should_exit = true;
		mainloop_quit();
		return;
	}

	mainloop_quit();
}

static void send_notification_raw(struct server *server, const uint8_t *data, size_t len, uint16_t char_handle)
{
	if (!server->gatt) {
		return;
	}

	uint16_t mtu = bt_gatt_server_get_mtu(server->gatt);
	size_t max_payload = mtu - 3;

	printf("[TX] handle=0x%04X len=%zu mtu=%u: ", char_handle, len, mtu);
	for (size_t i = 0; i < len; i++)
		printf("%02X ", data[i]);
	printf("\n");

	size_t offset = 0;
	while (offset < len) {
		size_t chunk = (len - offset > max_payload) ? max_payload : (len - offset);
		bool result = bt_gatt_server_send_notification(server->gatt, char_handle, data + offset, chunk,
								 false);
		if (!result) {
			printf("[TX] notification failed at offset=%zu\n", offset);
			break;
		}
		offset += chunk;
		if (offset < len) {
			usleep(50000);
		}
	}
}

static bool is_notify_enabled_for_handle(const struct server *server, uint16_t char_handle)
{
	if (char_handle == improv_chars.state_value_handle)
		return server->state_notify_enabled;

	if (char_handle == improv_chars.error_value_handle)
		return server->error_notify_enabled;

	if (char_handle == improv_chars.result_handle)
		return server->result_notify_enabled;

	return false;
}

static void dump_hex(const char *tag, const uint8_t *buf, size_t len)
{
	printf("[DUMP] %s len=%zu: ", tag, len);
	for (size_t i = 0; i < len; i++) {
		printf("%02X ", buf[i]);
	}
	printf("\n");
}

static uint8_t checksum8(const uint8_t *buf, size_t len)
{
	uint32_t sum = 0;
	for (size_t i = 0; i < len; i++)
		sum += buf[i];
	return (uint8_t)(sum & 0xFF);
}

static uint64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void improv_update_adv_state(void)
{
	if (!advertising)
		return;
	set_adv_data();
}

static void update_adv_mode(void)
{
	uint64_t now = monotonic_ms();

	if (last_name_adv_time_ms == 0) {
		last_name_adv_time_ms = now;
		adv_name_active = true;
		set_adv_data();
		return;
	}

	if (adv_name_active) {
		if (now - last_name_adv_time_ms >= NAME_ADVERTISING_DURATION_MS) {
			adv_name_active = false;
			set_adv_data();
		}
		return;
	}

	if (now - last_name_adv_time_ms >= NAME_ADVERTISING_INTERVAL_MS) {
		adv_name_active = true;
		last_name_adv_time_ms = now;
		set_adv_data();
	}
}

static void adv_rotate_cb(int timeout_id, void *user_data)
{
	if (!advertising)
		return;
	update_adv_mode();
	adv_rotate_timeout_id = mainloop_add_timeout(1000, adv_rotate_cb, NULL, NULL);
}

static void improv_send_result(struct server *server, uint8_t cmd, const char **strings, size_t count)
{
	uint8_t out[512];
	size_t idx = 0;

	out[idx++] = cmd;
	out[idx++] = 0; // data length placeholder

	for (size_t i = 0; i < count; i++) {
		size_t slen = strlen(strings[i]);
		out[idx++] = (uint8_t)slen;
		memcpy(&out[idx], strings[i], slen);
		idx += slen;
	}

	out[1] = (uint8_t)(idx - 2);
	out[idx++] = checksum8(out, idx);

	pthread_mutex_lock(&server->notification_lock);
	if (is_notify_enabled_for_handle(server, improv_chars.result_handle) && client_connected) {
		send_notification_raw(server, out, idx, improv_chars.result_handle);
	}
	pthread_mutex_unlock(&server->notification_lock);
}

static void improv_notify_state(struct server *server)
{
	const char *state_str[] = { "0x00", "Awaiting Auth(0x01)", "Authorized(0x02)", "Provisioning(0x03)", "Provisioned(0x04)" };
	const char *s = (improv_state <= 0x04) ? state_str[improv_state] : "Unknown";
	printf("[TX] Notify state -> %s\n", s);
	improv_update_adv_state();
	uint8_t val = improv_state;
	pthread_mutex_lock(&server->notification_lock);
	if (is_notify_enabled_for_handle(server, improv_chars.state_value_handle) && client_connected) {
		send_notification_raw(server, &val, 1, improv_chars.state_value_handle);
	} else {
		printf("[TX] state notify skipped (enabled=%d connected=%d)\n",
		       server->state_notify_enabled, client_connected);
	}
	pthread_mutex_unlock(&server->notification_lock);
}

static void improv_notify_error(struct server *server)
{
	const char *err_str[] = { "No Error(0x00)", "Invalid RPC(0x01)", "Unknown CMD(0x02)", "Unable to Connect(0x03)",
				  "Not Authorized(0x04)", "Unknown Error(0x05)" };
	const char *e = (improv_error <= 0x05) ? err_str[improv_error] : "Unknown";
	printf("[TX] Notify error -> %s\n", e);
	uint8_t val = improv_error;
	pthread_mutex_lock(&server->notification_lock);
	if (is_notify_enabled_for_handle(server, improv_chars.error_value_handle) && client_connected) {
		send_notification_raw(server, &val, 1, improv_chars.error_value_handle);
	} else {
		printf("[TX] error notify skipped (enabled=%d connected=%d)\n",
		       server->error_notify_enabled, client_connected);
	}
	pthread_mutex_unlock(&server->notification_lock);
}

static void improv_send_device_info(struct server *server)
{
	const char *fw_name = "ThirdReality";
	const char *fw_ver = "unknown";
	const char *hw = "trspk";
	const char *dev_name = get_device_name();
	const char *result_strs[] = { fw_name, fw_ver, hw, dev_name };

	printf("[TX] Device info: fw_name=%s fw_ver=%s hw=%s dev_name=%s\n", fw_name, fw_ver, hw, dev_name);
	improv_send_result(server, 0x03, result_strs, 4);
}

static void improv_handle_rpc_packet(struct server *server, const uint8_t *value, size_t len)
{
	dump_hex("RPC full packet", value, len);
	if (len < 3) {
		printf("[RX] RPC packet too short (len=%zu), sending error 0x01\n", len);
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}

	uint8_t cmd = value[0];
	uint8_t dlen = value[1];

	printf("[RX] RPC cmd=0x%02X dlen=%u total_len=%zu\n", cmd, dlen, len);

	if (dlen + 3 != len) {
		printf("[RX] RPC length mismatch: expected %u+3=%u but got %zu\n", dlen, dlen + 3, len);
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}

	if (checksum8(value, len - 1) != value[len - 1]) {
		printf("[RX] RPC checksum error: expected 0x%02X got 0x%02X\n",
		       checksum8(value, len - 1), value[len - 1]);
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}

	if (cmd == 0x02) {
		printf("[RX] CMD: Identify\n");
		improv_error = 0x00;
		improv_notify_error(server);
		return;
	} else if (cmd == 0x03) {
		printf("[RX] CMD: Get Device Info\n");
		improv_error = 0x00;
		improv_notify_error(server);
		improv_send_device_info(server);
		return;
	} else if (cmd != 0x01) {
		printf("[RX] CMD: Unknown command 0x%02X\n", cmd);
		improv_error = 0x02;
		improv_notify_error(server);
		return;
	}

	// Wi-Fi settings
	const uint8_t *p = &value[2];
	if (dlen < 2) {
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}

	uint8_t ssid_len = p[0];
	p++;
	if ((size_t)(1 + ssid_len + 1) > dlen) {
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}

	const char *ssid = (const char *)p;
	p += ssid_len;

	uint8_t pw_len = p[0];
	p++;
	if ((size_t)(1 + ssid_len + 1 + pw_len) > dlen) {
		improv_error = 0x01;
		improv_notify_error(server);
		return;
	}
	const char *pw = (const char *)p;

	char ssid_buf[128];
	char pw_buf[128];
	memset(ssid_buf, 0, sizeof(ssid_buf));
	memset(pw_buf, 0, sizeof(pw_buf));

	memcpy(ssid_buf, ssid, ssid_len);
	memcpy(pw_buf, pw, pw_len);

	printf("[RX] CMD: Send WiFi Settings - SSID='%s' PW='%s'\n",
	       ssid_buf, pw_len > 0 ? "(provided)" : "(empty)");

	improv_state = 0x03; // Provisioning
	improv_error = 0x00;
	improv_notify_state(server);
	improv_notify_error(server); // Clear any stale error from previous attempt

	char ip[64] = { 0 };
	if (process_wifi_config_improv(ssid_buf, pw_buf, ip, sizeof(ip)) == 0) {
		printf("[WiFi] Connected successfully, IP=%s\n", ip);
		improv_state = 0x04; // Provisioned
		improv_error = 0x00;
		improv_notify_state(server);

		char url[96];
		snprintf(url, sizeof(url), "http://%s", ip);
		printf("[TX] RPC result: url=%s\n", url);
		const char *result_strs[] = { url };
		improv_send_result(server, cmd, result_strs, 1);

		wifi_config_completed = true;
		mainloop_add_timeout(2000, delayed_exit_cb, NULL, NULL);
		} else {
			printf("[WiFi] Connection failed for SSID='%s'\n", ssid_buf);
			improv_state = 0x02; // Authorized
			improv_error = 0x03; // Unable to connect
			improv_notify_state(server);
		improv_notify_error(server);

			printf("[TX] RPC result: empty (failed)\n");
			const char *result_strs[] = { "" };
			improv_send_result(server, cmd, result_strs, 1);

			/* End this BLE session after reporting failure so the client can reconnect cleanly. */
			mainloop_add_timeout(500, delayed_session_reset_cb, NULL, NULL);
		}
	}

static void improv_write_rpc_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, const uint8_t *value,
				size_t len, uint8_t opcode, struct bt_att *att, void *user_data)
{
	struct server *server = user_data;

	if (len > 0 && value)
		dump_hex("RPC write chunk", value, len);
	if (opcode != BT_ATT_OP_WRITE_CMD)
		gatt_db_attribute_write_result(attrib, id, 0);

	if (opcode == BT_ATT_OP_PREP_WRITE_REQ) {
		if (len > 0) {
			if (offset + len > MAX_WRITE_BUFFER) {
				printf("[RPC] prep_write overflow offset=%u len=%zu\n", offset, len);
				server->write_buffer_len = 0;
				server->write_in_progress = false;
				return;
			}
			memcpy(server->write_buffer + offset, value, len);
			if (offset + len > server->write_buffer_len)
				server->write_buffer_len = offset + len;
		}
		server->write_in_progress = true;
		return;
	} else if (opcode == BT_ATT_OP_EXEC_WRITE_REQ) {
		if (len == 0) {
			if (!server->write_in_progress || server->write_buffer_len == 0) {
				printf("[RPC] exec_write with empty buffer\n");
				return;
			}
			dump_hex("RPC exec buffer", server->write_buffer, server->write_buffer_len);
			server->write_in_progress = false;
			improv_handle_rpc_packet(server, server->write_buffer, server->write_buffer_len);
			server->write_buffer_len = 0;
			return;
		}
		/* Some stacks send full data in EXEC_WRITE; treat it as normal write below. */
	}

	// WRITE_REQ or WRITE_CMD
	if (offset != 0)
		return;

	if (len + server->write_buffer_len > MAX_WRITE_BUFFER) {
		printf("[RPC] write overflow len=%zu buf_len=%zu\n", len, server->write_buffer_len);
		server->write_buffer_len = 0;
		return;
	}
	memcpy(server->write_buffer + server->write_buffer_len, value, len);
	server->write_buffer_len += len;

	// If we have at least cmd + len, compute total length
	if (server->write_buffer_len >= 2) {
		size_t total = (size_t)server->write_buffer[1] + 3;
		if (server->write_buffer_len >= total) {
			improv_handle_rpc_packet(server, server->write_buffer, total);
			// shift any extra bytes (unlikely)
			if (server->write_buffer_len > total) {
				memmove(server->write_buffer, server->write_buffer + total, server->write_buffer_len - total);
				server->write_buffer_len -= total;
			} else {
				server->write_buffer_len = 0;
			}
		}
	}
}

/* Use BlueZ's built-in UUID parsing for GATT services */
static void parse_uuid_string(const char *str, bt_uuid_t *uuid)
{
	bt_string_to_uuid(uuid, str);
}

/* Parse UUID string to raw bytes in little-endian for BLE advertising */
static void uuid_str_to_le_bytes(const char *str, uint8_t *out)
{
	if (strlen(str) != 36) {
		return;
	}

	/* Parse UUID string to big-endian bytes first */
	uint8_t tmp[16];
	int i = 0, j = 0;
	char buf[3] = { 0 };

	while (i < 36 && j < 16) {
		if (str[i] == '-') {
			i++;
			continue;
		}
		buf[0] = str[i++];
		buf[1] = str[i++];
		tmp[j++] = (uint8_t)strtoul(buf, NULL, 16);
	}

	/* Reverse to little-endian for BLE advertising */
	for (int k = 0; k < 16; k++) {
		out[k] = tmp[15 - k];
	}
}

static void cccd_read_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode, struct bt_att *att,
			 void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2] = { 0, 0 };
	uint16_t cccd_handle = gatt_db_attribute_get_handle(attrib);

	pthread_mutex_lock(&server->notification_lock);

	if (cccd_handle == improv_chars.state_value_handle + 1 && server->state_notify_enabled)
		value[0] = 0x01;
	else if (cccd_handle == improv_chars.error_value_handle + 1 && server->error_notify_enabled)
		value[0] = 0x01;
	else if (cccd_handle == improv_chars.result_handle + 1 && server->result_notify_enabled)
		value[0] = 0x01;
	else if (cccd_handle == muse_chars.tx_value_handle + 1 && server->muse_notify_enabled)
		value[0] = 0x01;

	pthread_mutex_unlock(&server->notification_lock);

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static void cccd_write_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, const uint8_t *value, size_t len,
			  uint8_t opcode, struct bt_att *att, void *user_data)
{
	uint16_t cccd_handle = gatt_db_attribute_get_handle(attrib);
	bool enabled;
	struct server *server = user_data;

	if (len != 2) {
		gatt_db_attribute_write_result(attrib, id, BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN);
		return;
	}

	uint16_t cccd_value = (value[1] << 8) | value[0];

	printf("[RX] CCCD write handle=0x%04X value=0x%04X (%s)\n", cccd_handle, cccd_value,
	       (cccd_value & 0x01) ? "Notify" : (cccd_value & 0x02) ? "Indicate" : "Disabled");

	enabled = !!(cccd_value & 0x03);

	pthread_mutex_lock(&server->notification_lock);

	if (cccd_handle == improv_chars.state_value_handle + 1)
		server->state_notify_enabled = enabled;
	else if (cccd_handle == improv_chars.error_value_handle + 1)
		server->error_notify_enabled = enabled;
	else if (cccd_handle == improv_chars.result_handle + 1)
		server->result_notify_enabled = enabled;
	else if (cccd_handle == muse_chars.tx_value_handle + 1)
		server->muse_notify_enabled = enabled;

	pthread_mutex_unlock(&server->notification_lock);
	gatt_db_attribute_write_result(attrib, id, 0);

	if (client_connected && enabled) {
		usleep(50000);
		if (cccd_handle == improv_chars.state_value_handle + 1)
			improv_notify_state(server);
		else if (cccd_handle == improv_chars.error_value_handle + 1)
			improv_notify_error(server);
	}
}

static void gap_device_name_read_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				    struct bt_att *att, void *user_data)
{
	const char *name = get_device_name();
	uint16_t len = strlen(name);

	if (offset > len) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}

	len -= offset;
	gatt_db_attribute_read_result(attrib, id, 0, (uint8_t *)(name + offset), len);
}

static void gap_appearance_read_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				   struct bt_att *att, void *user_data)
{
	uint8_t appearance[2] = { 0x00, 0x00 }; // Unknown appearance

	if (offset > 2) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}

	gatt_db_attribute_read_result(attrib, id, 0, appearance + offset, 2 - offset);
}

static void populate_gap_service(struct server *server)
{
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;

	bt_uuid16_create(&uuid, 0x1800);
	service = gatt_db_add_service(server->db, &uuid, true, 6);

	bt_uuid16_create(&uuid, 0x2A00);
	gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ, BT_GATT_CHRC_PROP_READ, gap_device_name_read_cb, NULL,
					   server);

	bt_uuid16_create(&uuid, 0x2A01);
	gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ, BT_GATT_CHRC_PROP_READ, gap_appearance_read_cb, NULL,
					   server);

	gatt_db_service_set_active(service, true);
}

static void populate_gatt_service(struct server *server)
{
	struct gatt_db_attribute *service, *characteristic;
	bt_uuid_t uuid;

	bt_uuid16_create(&uuid, 0x1801);
	/* Simplified: only Service Changed, no extra features */
	service = gatt_db_add_service(server->db, &uuid, true, 4);

	// Service Changed (0x2A05) - Indicate only (required by BT spec)
	bt_uuid16_create(&uuid, 0x2A05);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ, BT_GATT_CHRC_PROP_INDICATE, NULL,
						    NULL, server);
	gatt_db_service_add_ccc(characteristic, BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);

	gatt_db_service_set_active(service, true);
}

static void improv_read_caps_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				struct bt_att *att, void *user_data)
{
	if (offset > 1) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}
	uint8_t val = improv_caps;
	gatt_db_attribute_read_result(attrib, id, 0, &val, 1);
}

static void improv_read_state_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				 struct bt_att *att, void *user_data)
{
	if (offset > 1) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}
	uint8_t val = improv_state;
	gatt_db_attribute_read_result(attrib, id, 0, &val, 1);
}

static void improv_read_error_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				 struct bt_att *att, void *user_data)
{
	if (offset > 1) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}
	uint8_t val = improv_error;
	gatt_db_attribute_read_result(attrib, id, 0, &val, 1);
}

static void improv_read_result_cb(struct gatt_db_attribute *attrib, unsigned int id, uint16_t offset, uint8_t opcode,
				  struct bt_att *att, void *user_data)
{
	/* Result is empty until RPC command is processed */
	gatt_db_attribute_read_result(attrib, id, 0, NULL, 0);
}


static void muse_notify_json(struct server *server, const uint8_t *data, size_t len)
{
	uint16_t mtu = bt_gatt_server_get_mtu(server->gatt);
	size_t usable = mtu > 6 ? mtu - 6 : 0;
	size_t total, i;
	uint8_t packet[520];

	if (!server->muse_notify_enabled || usable == 0)
		return;
	if (usable > sizeof(packet) - 3)
		usable = sizeof(packet) - 3;
	total = (len + usable - 1) / usable;
	if (total == 0 || total > 255)
		return;
	for (i = 0; i < total; i++) {
		size_t offset = i * usable;
		size_t part = len - offset > usable ? usable : len - offset;
		packet[0] = MUSE_CHUNK_MAGIC;
		packet[1] = (uint8_t)i;
		packet[2] = (uint8_t)total;
		memcpy(packet + 3, data + offset, part);
		bt_gatt_server_send_notification(server->gatt, muse_chars.tx_value_handle,
						 packet, part + 3, false);
		if (i + 1 < total)
			usleep(50000);
	}
}

static void muse_dispatch(struct server *server, const uint8_t *data, size_t len)
{
	struct sockaddr_un local = { 0 }, remote = { 0 };
	struct timeval first = { .tv_sec = 10 };
	struct timeval next = { .tv_sec = 2 };
	char local_path[sizeof(local.sun_path)];
	uint8_t response[MUSE_MAX_MESSAGE];
	int fd;
	ssize_t n;
	bool got_any = false;

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		static const uint8_t error[] = "{\"status\":\"error_pairing_unavailable\"}";
		muse_notify_json(server, error, sizeof(error) - 1);
		return;
	}
	snprintf(local_path, sizeof(local_path), "/tmp/muse-gatt-%ld.sock", (long)getpid());
	unlink(local_path);
	local.sun_family = AF_UNIX;
	strncpy(local.sun_path, local_path, sizeof(local.sun_path) - 1);
	remote.sun_family = AF_UNIX;
	strncpy(remote.sun_path, MUSE_CONTROL_SOCKET, sizeof(remote.sun_path) - 1);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &first, sizeof(first));
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    sendto(fd, data, len, MSG_NOSIGNAL,
		   (struct sockaddr *)&remote, sizeof(remote)) < 0) {
		close(fd);
		unlink(local_path);
		static const uint8_t error[] = "{\"status\":\"error_pairing_unavailable\"}";
		muse_notify_json(server, error, sizeof(error) - 1);
		return;
	}

	/*
	 * One request may yield several responses (e.g. provision_v2 streams
	 * wifi_connecting -> wifi_connected -> auth_ok). Forward each datagram
	 * as it arrives, then stop after a short idle gap.
	 */
	for (;;) {
		n = recv(fd, response, sizeof(response), 0);
		if (n <= 0)
			break;
		muse_notify_json(server, response, (size_t)n);
		got_any = true;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &next, sizeof(next));
	}

	close(fd);
	unlink(local_path);
	if (!got_any) {
		static const uint8_t error[] = "{\"status\":\"error_pairing_unavailable\"}";
		muse_notify_json(server, error, sizeof(error) - 1);
	}
}

static void muse_rx_write_cb(struct gatt_db_attribute *attrib, unsigned int id,
			     uint16_t offset, const uint8_t *value, size_t len,
			     uint8_t opcode, struct bt_att *att, void *user_data)
{
	struct server *server = user_data;
	(void)opcode;
	(void)att;
	if (offset != 0 || len == 0) {
		gatt_db_attribute_write_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET);
		return;
	}
	if (value[0] != MUSE_CHUNK_MAGIC) {
		muse_dispatch(server, value, len);
	} else if (len >= 3) {
		uint8_t index = value[1], total = value[2];
		if (index == 0) {
			server->muse_buffer_len = 0;
			server->muse_chunk_count = total;
			server->muse_next_chunk = 0;
		}
		if (total == 0 || total != server->muse_chunk_count ||
		    index != server->muse_next_chunk ||
		    server->muse_buffer_len + len - 3 > sizeof(server->muse_buffer)) {
			server->muse_buffer_len = 0;
			server->muse_chunk_count = 0;
		} else {
			memcpy(server->muse_buffer + server->muse_buffer_len, value + 3, len - 3);
			server->muse_buffer_len += len - 3;
			server->muse_next_chunk++;
			if (server->muse_next_chunk == total) {
				muse_dispatch(server, server->muse_buffer, server->muse_buffer_len);
				server->muse_buffer_len = 0;
				server->muse_chunk_count = 0;
			}
		}
	}
	gatt_db_attribute_write_result(attrib, id, 0);
}

static void muse_tx_read_cb(struct gatt_db_attribute *attrib, unsigned int id,
			    uint16_t offset, uint8_t opcode, struct bt_att *att,
			    void *user_data)
{
	(void)opcode;
	(void)att;
	(void)user_data;
	if (offset != 0) {
		gatt_db_attribute_read_result(attrib, id, BT_ATT_ERROR_INVALID_OFFSET, NULL, 0);
		return;
	}
	gatt_db_attribute_read_result(attrib, id, 0, NULL, 0);
}

static void populate_muse_service(struct server *server)
{
	struct gatt_db_attribute *service, *characteristic;
	bt_uuid_t uuid;
	parse_uuid_string(MUSE_SERVICE_UUID_STR, &uuid);
	service = gatt_db_add_service(server->db, &uuid, true, 8);
	if (!service)
		return;
	parse_uuid_string(MUSE_RX_UUID_STR, &uuid);
	muse_chars.rx = gatt_db_service_add_characteristic(
		service, &uuid, BT_ATT_PERM_WRITE,
		BT_GATT_CHRC_PROP_WRITE | BT_GATT_CHRC_PROP_WRITE_WITHOUT_RESP,
		NULL, muse_rx_write_cb, server);
	parse_uuid_string(MUSE_TX_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(
		service, &uuid, BT_ATT_PERM_READ,
		BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
		muse_tx_read_cb, NULL, server);
	gatt_db_service_add_ccc(characteristic, BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);
	muse_chars.tx = characteristic;
	muse_chars.tx_value_handle = gatt_db_attribute_get_handle(characteristic);
	gatt_db_service_set_active(service, true);
}

static void populate_improv_service(struct server *server)
{
	struct gatt_db_attribute *service, *characteristic;
	bt_uuid_t uuid;

	gatt_db_ccc_register(server->db, cccd_read_cb, cccd_write_cb, NULL, server);

	parse_uuid_string(IMPROV_SERVICE_UUID_STR, &uuid);

	/*
	 * IMPORTANT: Allocate enough handles for all characteristics + descriptors.
	 * Each characteristic needs: 1 (declaration) + 1 (value) = 2 handles minimum
	 * Each CCCD descriptor needs: 1 handle
	 * 
	 * We have 5 characteristics:
	 * - Current State: 2 (char) + 1 (CCCD) = 3 handles
	 * - Error State: 2 (char) + 1 (CCCD) = 3 handles
	 * - RPC Command: 2 (char) = 2 handles
	 * - RPC Result: 2 (char) + 1 (CCCD) = 3 handles
	 * - Capabilities: 2 (char) = 2 handles
	 * Total: 13 handles + 1 (service declaration) = 14, use 20 for safety
	 */
	service = gatt_db_add_service(server->db, &uuid, true, 20);
	if (!service) {
		return;
	}

	/*
	 * CRITICAL FIX: Register characteristics in UUID order (8001, 8002, 8003, 8004, 8005)
	 * to match Improv WiFi specification and what Android Improv SDK expects.
	 * Some Android BLE stacks may depend on characteristic order within a service.
	 */

	// 1. Current State (UUID: ...8001) - read + notify - per Improv spec
	parse_uuid_string(IMPROV_CHAR_STATE_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ,
								    BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
								    improv_read_state_cb, NULL, server);
	gatt_db_service_add_ccc(characteristic, BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);
	improv_chars.state = characteristic;
	improv_chars.state_value_handle = gatt_db_attribute_get_handle(characteristic);

	// 2. Error State (UUID: ...8002) - read + notify - per Improv spec
	parse_uuid_string(IMPROV_CHAR_ERROR_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ,
								    BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
								    improv_read_error_cb, NULL, server);
	gatt_db_service_add_ccc(characteristic, BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);
	improv_chars.error = characteristic;
	improv_chars.error_value_handle = gatt_db_attribute_get_handle(characteristic);

	// 3. RPC Command (UUID: ...8003) - write only - per Improv spec
	// IMPORTANT: prefer Write Request to allow long writes (prepare/execute) when MTU is small
	parse_uuid_string(IMPROV_CHAR_RPC_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_WRITE,
				    BT_GATT_CHRC_PROP_WRITE,
						    NULL, improv_write_rpc_cb, server);
	improv_chars.rpc = characteristic;

	// 4. RPC Result (UUID: ...8004) - read + notify - per Improv spec
	parse_uuid_string(IMPROV_CHAR_RESULT_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ,
							    BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_NOTIFY,
							    improv_read_result_cb, NULL, server);
	gatt_db_service_add_ccc(characteristic, BT_ATT_PERM_READ | BT_ATT_PERM_WRITE);
	improv_chars.result = characteristic;
	improv_chars.result_handle = gatt_db_attribute_get_handle(characteristic);

	// 5. Capabilities (UUID: ...8005) - read only - per Improv spec
	parse_uuid_string(IMPROV_CHAR_CAPS_UUID_STR, &uuid);
	characteristic = gatt_db_service_add_characteristic(service, &uuid, BT_ATT_PERM_READ, BT_GATT_CHRC_PROP_READ,
							    improv_read_caps_cb, NULL, server);
	improv_chars.cap = characteristic;
	if (!gatt_db_service_set_active(service, true)) {
		return;
	}
}

static struct server *server_create(int fd)
{
	struct server *server;

	server = malloc(sizeof(*server));
	if (!server) {
		return NULL;
	}

	memset(server, 0, sizeof(*server));
	server->fd = fd;

	server->att = bt_att_new(fd, false);
	if (!server->att) {
		free(server);
		return NULL;
	}

	if (!bt_att_set_close_on_unref(server->att, true)) {
		bt_att_unref(server->att);
		free(server);
		return NULL;
	}

	bt_att_register_disconnect(server->att, att_disconnect_cb, server, NULL);

	/* 
	 * Use default MTU (23 bytes). Do NOT force a large MTU before negotiation!
	 * Android will negotiate MTU via Exchange MTU Request if needed.
	 * Forcing large MTU causes service discovery responses to be truncated.
	 */

	server->db = gatt_db_new();
	if (!server->db) {
		bt_att_unref(server->att);
		free(server);
		return NULL;
	}

	/* Use default MTU of 23 bytes, let client negotiate higher if needed */
	server->gatt = bt_gatt_server_new(server->db, server->att, 23, 0);
	if (!server->gatt) {
		gatt_db_unref(server->db);
		bt_att_unref(server->att);
		free(server);
		return NULL;
	}

	server->state_notify_enabled = false;
	server->error_notify_enabled = false;
	server->result_notify_enabled = false;
	pthread_mutex_init(&server->notification_lock, NULL);

	populate_gap_service(server);
	populate_gatt_service(server);
	populate_improv_service(server);
	populate_muse_service(server);

	return server;
}

static void server_destroy(struct server *server)
{
	bt_gatt_server_unref(server->gatt);
	gatt_db_unref(server->db);
}

static int l2cap_le_att_listen_and_accept(bdaddr_t *src, int sec, uint8_t src_type)
{
	int sk, nsk;
	struct sockaddr_l2 srcaddr, addr;
	socklen_t optlen;
	struct bt_security btsec;

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = src_type;
	bacpy(&srcaddr.l2_bdaddr, src);
	if (bind(sk, (struct sockaddr *)&srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		goto fail;
	}

	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec, sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		goto fail;
	}

	if (listen(sk, 10) < 0) {
		perror("Listening on socket failed");
		goto fail;
	}

	fd_set readfds;
	int max_fd = sk;
	time_t start_time = time(NULL);

	sigset_t empty_mask;
	sigemptyset(&empty_mask);

	while (!should_exit) {
		FD_ZERO(&readfds);
		FD_SET(sk, &readfds);

		struct timespec timeout;
		timeout.tv_sec = 1;
		timeout.tv_nsec = 0;

		int ret = pselect(max_fd + 1, &readfds, NULL, NULL, &timeout, &empty_mask);

		time_t current_time = time(NULL);
		if (current_time - start_time >= NO_CLIENT_TIMEOUT_SECONDS) {
			should_exit = true;
			goto fail;
		}

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("Select failed");
			goto fail;
		} else if (ret == 0) {
			if (should_exit) {
				break;
			}
			continue;
		} else if (FD_ISSET(sk, &readfds)) {
			break;
		}
	}

	if (should_exit) {
		goto fail;
	}

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);
	nsk = accept(sk, (struct sockaddr *)&addr, &optlen);
	if (nsk < 0) {
		perror("Accept failed");
		goto fail;
	}

	close(sk);

	return nsk;

fail:
	close(sk);
	return -1;
}

static void signal_handler(int signum)
{
	switch (signum) {
	case SIGINT: {
		should_exit = true;
		mainloop_quit();
		break;
	}
	case SIGTERM: {
		if (advertising) {
			struct bt_hci_cmd_le_set_adv_enable param;
			param.enable = 0;

			int hdev = hci_get_route(NULL);
			if (hdev >= 0) {
				int dd = hci_open_dev(hdev);
				if (dd >= 0) {
					struct hci_request rq;
					uint8_t status;
					memset(&rq, 0, sizeof(rq));
					rq.ogf = OGF_LE_CTL;
					rq.ocf = BT_HCI_CMD_LE_SET_ADV_ENABLE;
					rq.cparam = &param;
					rq.clen = sizeof(param);
					rq.rparam = &status;
					rq.rlen = 1;
					hci_send_req(dd, &rq, 1000);
					hci_close_dev(dd);
				}
			}
			advertising = false;
		}

		should_exit = true;
		mainloop_quit();
		break;
	}
	default:
		break;
	}
}

static void signal_cb(int signum, void *user_data)
{
	(void)user_data;
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		should_exit = true;
		mainloop_quit();
		break;
	default:
		break;
	}
}

static void send_cmd(int cmd, void *params, int params_len)
{
	struct hci_request rq;
	uint8_t status;
	int dd, hdev;

	hdev = hci_get_route(NULL);
	if (hdev < 0) {
		perror("Could not get HCI device");
		exit(1);
	}

	dd = hci_open_dev(hdev);
	if (dd < 0) {
		perror("Could not open device");
		exit(1);
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf = OGF_LE_CTL;
	rq.ocf = cmd;
	rq.cparam = params;
	rq.clen = params_len;
	rq.rparam = &status;
	rq.rlen = 1;

	hci_send_req(dd, &rq, 1000);
	hci_close_dev(dd);
}

static char device_name_cache[32] = { 0 };
static bool device_name_initialized = false;

static const char *get_device_name(void)
{
	if (device_name_initialized) {
		return device_name_cache;
	}

	/* Build device name from /data/conf/device.json: 3RSPK-xxxxxx */
	const char *default_name = "3RSPK-000000";
	FILE *fp = fopen("/data/conf/device.json", "r");
	if (!fp) {
		snprintf(device_name_cache, sizeof(device_name_cache), "%s", default_name);
		device_name_initialized = true;
		return device_name_cache;
	}

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	char *content = malloc(size + 1);
	if (!content) {
		fclose(fp);
		snprintf(device_name_cache, sizeof(device_name_cache), "%s", default_name);
		device_name_initialized = true;
		return device_name_cache;
	}

	fread(content, 1, size, fp);
	content[size] = '\0';
	fclose(fp);

	cJSON *root = cJSON_Parse(content);
	free(content);
	if (!root) {
		snprintf(device_name_cache, sizeof(device_name_cache), "%s", default_name);
		device_name_initialized = true;
		return device_name_cache;
	}

	cJSON *device = cJSON_GetObjectItem(root, "device");
	cJSON *mac = device ? cJSON_GetObjectItem(device, "macAddress") : NULL;
	if (mac && cJSON_IsString(mac) && mac->valuestring) {
		char compact[13] = { 0 };
		int ci = 0;
		for (const char *p = mac->valuestring; *p && ci < 12; p++) {
			if (*p == ':')
				continue;
			compact[ci++] = *p;
		}
		if (ci >= 6) {
			const char *last6 = compact + (ci - 6);
			snprintf(device_name_cache, sizeof(device_name_cache), "3RSPK-%s", last6);
		} else {
			snprintf(device_name_cache, sizeof(device_name_cache), "%s", default_name);
		}
	} else {
		snprintf(device_name_cache, sizeof(device_name_cache), "%s", default_name);
	}

	cJSON_Delete(root);
	device_name_initialized = true;
	return device_name_cache;
}

static void set_adv_parameters(void)
{
	struct bt_hci_cmd_le_set_adv_parameters param;

	param.own_addr_type = 0x00; /* Use public address */
	param.min_interval = cpu_to_le16(0x00A0); // ~100ms
	param.max_interval = cpu_to_le16(0x00F0); // ~150ms
	param.type = 0x00; /* connectable no-direct advertising */
	param.direct_addr_type = 0x00;
	memset(param.direct_addr, 0, 6);
	param.channel_map = 0x07;
	param.filter_policy = 0x00;

	send_cmd(BT_HCI_CMD_LE_SET_ADV_PARAMETERS, (void *)&param, sizeof(param));
}

static void set_adv_enable(int enable)
{
	struct bt_hci_cmd_le_set_adv_enable param;
	if (enable != 0 && enable != 1) {
		return;
	}
	param.enable = enable;
	send_cmd(BT_HCI_CMD_LE_SET_ADV_ENABLE, (void *)&param, sizeof(param));
}

static void set_adv_response(void)
{
	struct bt_hci_cmd_le_set_scan_rsp_data param;
	const char *device_name = access("/data/conf/muse/sdk-token", R_OK) == 0
		? "MuseGadget-3RSPK" : get_device_name();

	memset(&param, 0, sizeof(param));
	param.len = 0;

	size_t name_len = strlen(device_name);
	if (name_len > 29) {
		name_len = 29;
	}

	// Complete Local Name in scan response (full device name)
	param.data[param.len++] = name_len + 1;
	param.data[param.len++] = 0x09; // Complete Local Name
	memcpy(&param.data[param.len], device_name, name_len);
	param.len += name_len;

	send_cmd(BT_HCI_CMD_LE_SET_SCAN_RSP_DATA, &param, sizeof(param));
}

static void set_adv_data(void)
{
	struct bt_hci_cmd_le_set_adv_data param;

	memset(&param, 0, sizeof(param));
	param.len = 0;

	// Flags (3 bytes): LE General Discoverable + BR/EDR Not Supported
	param.data[param.len++] = 2;
	param.data[param.len++] = 0x01;
	param.data[param.len++] = 0x06;

	// Advertise Muse when configured; both services remain in the GATT database.
	uint8_t uuid_bytes[16];
	const char *advertised_uuid = access("/data/conf/muse/sdk-token", R_OK) == 0
		? MUSE_SERVICE_UUID_STR : IMPROV_SERVICE_UUID_STR;
	uuid_str_to_le_bytes(advertised_uuid, uuid_bytes);
	param.data[param.len++] = 17;
	param.data[param.len++] = 0x07;  // Complete List of 128-bit Service UUIDs
	memcpy(&param.data[param.len], uuid_bytes, 16);
	param.len += 16;

	// Improv Service Data (10 bytes): UUID(2) + header(2) + state(1) + caps(1) + reserved(4)
	param.data[param.len++] = 9;
	param.data[param.len++] = 0x16; // Service Data - 16-bit UUID
	param.data[param.len++] = 0x77; // UUID 0x4677 (LSB) - Improv
	param.data[param.len++] = 0x46; // UUID 0x4677 (MSB)
	param.data[param.len++] = 'I';  // Improv header
	param.data[param.len++] = 'M';
	param.data[param.len++] = improv_state;
	param.data[param.len++] = improv_caps;
	param.data[param.len++] = 0x00; // Reserved
	param.data[param.len++] = 0x00;

	send_cmd(BT_HCI_CMD_LE_SET_ADV_DATA, &param, sizeof(param));
}

void hci_dev_init(void)
{
	/* Open HCI socket */
	if ((ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI)) < 0) {
		perror("Can't open HCI socket.");
		exit(1);
	}

	hdi.dev_id = 0;

	if (ioctl(ctl, HCIGETDEVINFO, (void *)&hdi)) {
		perror("Can't get device info");
		exit(1);
	}
}

// Advertising control functions
static void start_advertising(void)
{
	if (!advertising) {
		adv_name_active = false;
		last_name_adv_time_ms = 0;
		set_adv_enable(0);
		set_adv_data();
		set_adv_parameters();
		set_adv_response();
		set_adv_enable(1);

		advertising = true;
	}
}

static void update_advertising_data(void)
{
	if (!advertising)
		return;
	set_adv_data();
}

static void stop_advertising(void)
{
	if (advertising) {
		if (adv_rotate_timeout_id > 0) {
			mainloop_remove_timeout(adv_rotate_timeout_id);
			adv_rotate_timeout_id = 0;
		}
		system("hcitool -i hci0 cmd 0x08 0x000A 01");
		set_adv_enable(0);

		struct bt_hci_cmd_le_set_adv_data adv_clear;
		memset(&adv_clear, 0, sizeof(adv_clear));
		adv_clear.len = 0;
		send_cmd(BT_HCI_CMD_LE_SET_ADV_DATA, &adv_clear, sizeof(adv_clear));

		struct bt_hci_cmd_le_set_scan_rsp_data scan_clear;
		memset(&scan_clear, 0, sizeof(scan_clear));
		scan_clear.len = 0;
		send_cmd(BT_HCI_CMD_LE_SET_SCAN_RSP_DATA, &scan_clear, sizeof(scan_clear));

		advertising = false;
	}
}

static void no_client_timeout_cb(int timeout_id, void *user_data)
{
	(void)timeout_id;
	(void)user_data;
	should_exit = true;
	mainloop_quit();
}

static void reset_no_client_timeout(void)
{
	if (no_client_timeout_id > 0) {
		mainloop_remove_timeout(no_client_timeout_id);
		no_client_timeout_id = 0;
	}

	if (!client_connected) {
		no_client_timeout_id = mainloop_add_timeout(NO_CLIENT_TIMEOUT_SECONDS * 1000, no_client_timeout_cb, NULL, NULL);
	}
}

static void cleanup_on_exit(void)
{
	if (advertising) {
		struct bt_hci_cmd_le_set_adv_enable param;
		param.enable = 0;

		int hdev = hci_get_route(NULL);
		if (hdev >= 0) {
			int dd = hci_open_dev(hdev);
			if (dd >= 0) {
				struct hci_request rq;
				uint8_t status;
				memset(&rq, 0, sizeof(rq));
				rq.ogf = OGF_LE_CTL;
				rq.ocf = BT_HCI_CMD_LE_SET_ADV_ENABLE;
				rq.cparam = &param;
				rq.clen = sizeof(param);
				rq.rparam = &status;
				rq.rlen = 1;
				hci_send_req(dd, &rq, 1000);
				hci_close_dev(dd);
			}
		}
		advertising = false;
	}
}

int main(void)
{
	bdaddr_t src_addr;
	int fd;
	int sec = BT_SECURITY_LOW;
	uint8_t src_type = BDADDR_LE_PUBLIC;

	atexit(cleanup_on_exit);

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction SIGINT");
		return EXIT_FAILURE;
	}

	if (sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("sigaction SIGTERM");
		return EXIT_FAILURE;
	}

	sigset_t blocked_signals;
	sigemptyset(&blocked_signals);
	sigaddset(&blocked_signals, SIGINT);
	sigaddset(&blocked_signals, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &blocked_signals, NULL) == -1) {
		perror("sigprocmask");
		return EXIT_FAILURE;
	}

	hci_dev_init();

	while (!should_exit) {
		if (sigaction(SIGINT, &sa, NULL) == -1) {
			perror("sigaction SIGINT re-install");
		}
		if (sigaction(SIGTERM, &sa, NULL) == -1) {
			perror("sigaction SIGTERM re-install");
		}

		start_advertising();

		bacpy(&src_addr, BDADDR_ANY);
		fd = l2cap_le_att_listen_and_accept(&src_addr, sec, src_type);
		if (fd < 0) {
			fprintf(stderr, "Failed to accept L2CAP ATT connection\n");
			usleep(500000);
			return EXIT_FAILURE;
		}

		stop_advertising();
		client_connected = true;
		printf("[BLE] Client connected\n");

		mainloop_init();

		server = server_create(fd);
		if (!server) {
			close(fd);
			usleep(500000);
			return EXIT_FAILURE;
		}

		reset_no_client_timeout();

		mainloop_run_with_signal(signal_cb, NULL);

		if (no_client_timeout_id > 0) {
			mainloop_remove_timeout(no_client_timeout_id);
			no_client_timeout_id = 0;
		}

		client_connected = false;
		advertising = false;
		reset_improv_session_state();

		server_destroy(server);
		server = NULL;

		if (should_exit) {
			break;
		}

		sleep(1);
	}

	if (no_client_timeout_id > 0) {
		mainloop_remove_timeout(no_client_timeout_id);
	}

	stop_advertising();
	if (server)
		server_destroy(server);

	usleep(800000);
	return EXIT_SUCCESS;
}
