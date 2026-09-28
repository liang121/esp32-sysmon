#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

int wifi_next_network(int count, int current, bool link_up, unsigned consecutive_failures);
void wifi_promote(char ssids[2][33], char passwords[2][65], int *count,
                  const char *ssid, const char *password);
void wifi_copy_ssid(unsigned char dest[32], const char *source);
bool wifi_got_ip_matches(const char *expected_ssid, const unsigned char connected_ssid[32],
                         uint32_t event_ip, uint32_t current_ip);

static void test_mac_failure_keeps_the_wifi_network(void) {
    assert(wifi_next_network(2, 0, true, 20) == -1);
    assert(wifi_next_network(2, 1, true, 20) == -1);
}

static void test_transient_link_failure_retries_current_before_fallback(void) {
    assert(wifi_next_network(2, 0, false, 1) == 0);
    assert(wifi_next_network(2, 0, false, 2) == 1);
    assert(wifi_next_network(2, 1, false, 3) == 1);
    assert(wifi_next_network(2, 1, false, 4) == 0);
    assert(wifi_next_network(1, 0, false, 9) == 0);
    assert(wifi_next_network(0, 0, false, 1) == -1);
}

static void test_successful_new_network_retains_old_one(void) {
    char ssids[2][33] = {"old-network", "older-network"};
    char passwords[2][65] = {"old-passphrase", "older-passphrase"};
    int count = 2;
    wifi_promote(ssids, passwords, &count, "new-network", "new-passphrase");
    assert(count == 2);
    assert(strcmp(ssids[0], "new-network") == 0);
    assert(strcmp(passwords[0], "new-passphrase") == 0);
    assert(strcmp(ssids[1], "old-network") == 0);
    assert(strcmp(passwords[1], "old-passphrase") == 0);
}

static void test_reselecting_current_network_does_not_duplicate_it(void) {
    char ssids[2][33] = {"current-network", "backup-network"};
    char passwords[2][65] = {"current-passphrase", "backup-passphrase"};
    int count = 2;
    wifi_promote(ssids, passwords, &count, "current-network", "new-passphrase");
    assert(count == 2);
    assert(strcmp(ssids[0], "current-network") == 0);
    assert(strcmp(passwords[0], "new-passphrase") == 0);
    assert(strcmp(ssids[1], "backup-network") == 0);
}

static void test_full_length_ssid_is_not_truncated(void) {
    const char *ssid = "12345678901234567890123456789012";
    unsigned char wifi_config_ssid[32] = {0};
    wifi_copy_ssid(wifi_config_ssid, ssid);
    assert(memcmp(wifi_config_ssid, ssid, 32) == 0);
}

static void test_old_ip_event_cannot_confirm_a_candidate(void) {
    unsigned char connected[32] = {0};
    wifi_copy_ssid(connected, "previous-network");
    assert(!wifi_got_ip_matches("new-network", connected, 0x01020304, 0x01020304));
    wifi_copy_ssid(connected, "new-network");
    assert(!wifi_got_ip_matches("new-network", connected, 0x01020304, 0x05060708));
    assert(!wifi_got_ip_matches("new-network", connected, 0, 0));
    assert(wifi_got_ip_matches("new-network", connected, 0x05060708, 0x05060708));
}

int main(void) {
    test_mac_failure_keeps_the_wifi_network();
    test_transient_link_failure_retries_current_before_fallback();
    test_successful_new_network_retains_old_one();
    test_reselecting_current_network_does_not_duplicate_it();
    test_full_length_ssid_is_not_truncated();
    test_old_ip_event_cannot_confirm_a_candidate();
}
