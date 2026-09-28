#include "wifi_policy.h"

#include <stdio.h>
#include <string.h>

int wifi_next_network(int count, int current, bool link_up, unsigned consecutive_failures) {
    if (link_up || count < 1 || current < 0 || current >= count) return -1;
    return count == 1 || consecutive_failures % 2 ? current : (current + 1) % count;
}

void wifi_promote(char ssids[2][33], char passwords[2][65], int *count,
                  const char *ssid, const char *password) {
    char old_ssid[33] = {0};
    char old_password[65] = {0};
    bool same_as_primary = *count > 0 && strcmp(ssids[0], ssid) == 0;
    if (*count > 0) {
        snprintf(old_ssid, sizeof old_ssid, "%s", ssids[0]);
        snprintf(old_password, sizeof old_password, "%s", passwords[0]);
    }
    if (!same_as_primary && *count > 0) {
        snprintf(ssids[1], sizeof ssids[1], "%s", old_ssid);
        snprintf(passwords[1], sizeof passwords[1], "%s", old_password);
        *count = 2;
    } else if (*count == 0) {
        *count = 1;
    }
    snprintf(ssids[0], sizeof ssids[0], "%s", ssid);
    snprintf(passwords[0], sizeof passwords[0], "%s", password);
    memset(old_password, 0, sizeof old_password);
}

void wifi_copy_ssid(uint8_t dest[32], const char *source) {
    memset(dest, 0, 32);
    for (size_t i = 0; i < 32 && source[i]; ++i) dest[i] = (uint8_t)source[i];
}

bool wifi_got_ip_matches(const char *expected_ssid, const uint8_t connected_ssid[32],
                         uint32_t event_ip, uint32_t current_ip) {
    uint8_t expected[32];
    wifi_copy_ssid(expected, expected_ssid);
    return event_ip != 0 && event_ip == current_ip && memcmp(expected, connected_ssid, 32) == 0;
}
