#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void ssid_format(const uint8_t *ssid, size_t len, char *out, size_t out_len,
                 bool (*supports)(uint32_t codepoint));

static bool supported(uint32_t codepoint) {
    return codepoint != 0x4ea7 && codepoint != 0x53d1;
}

int main(void) {
    char text[160];
    const char *network = "demo-产品研发";
    ssid_format((const uint8_t *)network, strlen(network), text, sizeof text, supported);
    assert(strcmp(text, "demo-<E4BAA7>品研<E58F91>") == 0);

    const uint8_t invalid[] = {'A', 0xff, 'B'};
    ssid_format(invalid, sizeof invalid, text, sizeof text, supported);
    assert(strcmp(text, "A<FF>B") == 0);
}
