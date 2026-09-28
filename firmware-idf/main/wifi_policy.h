#pragma once

#include <stdbool.h>
#include <stdint.h>

// Returns -1 when no Wi-Fi switch is needed, otherwise the saved network to try.
int wifi_next_network(int count, int current, bool link_up, unsigned consecutive_failures);

// Call only after the selected network has an IP address and NVS commit succeeds.
void wifi_promote(char ssids[2][33], char passwords[2][65], int *count,
                  const char *ssid, const char *password);

void wifi_copy_ssid(uint8_t dest[32], const char *source);

// Accept an IP event only for the associated AP and the netif's current address.
bool wifi_got_ip_matches(const char *expected_ssid, const uint8_t connected_ssid[32],
                         uint32_t event_ip, uint32_t current_ip);
