/**
 * @file raven_wifi.h
 * @brief Wi-Fi station and local SD-card HTTP manager for the Elecrow HMI.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/** Maximum nearby Wi-Fi networks retained for the touchscreen picker. */
#define RAVEN_WIFI_MAX_NETWORKS 20

/** One nearby Wi-Fi network presented to the gallery application. */
typedef struct {
    char ssid[33];
    int8_t rssi;
} raven_wifi_network_t;

/** Current connection state presented by the touch UI. */
typedef enum {
    RAVEN_WIFI_IDLE,
    RAVEN_WIFI_SCANNING,
    RAVEN_WIFI_CONNECTING,
    RAVEN_WIFI_CONNECTED,
    RAVEN_WIFI_FAILED,
} raven_wifi_state_t;

/** A snapshot of Wi-Fi state safe to read from the Raven rendering task. */
typedef struct {
    raven_wifi_state_t state;
    char ip[16];
    char message[96];
} raven_wifi_status_t;

/** Initialize ESP-NETIF, the station interface, and the local HTTP service. */
int raven_wifi_init(void);
/** Begin a connection attempt using the supplied WPA/WPA2 station credentials. */
int raven_wifi_connect(const char* ssid, const char* password);
/** Begin an asynchronous scan for nearby visible Wi-Fi networks. */
int raven_wifi_start_scan(void);
/** Copy the most recently completed scan result into @p networks. */
int raven_wifi_get_networks(raven_wifi_network_t* networks, int capacity);
/** Copy the latest station status and IPv4 address. */
void raven_wifi_get_status(raven_wifi_status_t* status);
/**
 * @brief Return and clear a flag raised after a browser changes card contents.
 *
 * The Raven task calls this to refresh its gallery list without drawing from
 * the HTTP server task.
 */
int raven_wifi_take_media_changed(void);
