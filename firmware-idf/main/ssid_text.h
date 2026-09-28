#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Preserve the original SSID for connecting; format only its on-screen label.
void ssid_format(const uint8_t *ssid, size_t len, char *out, size_t out_len,
                 bool (*supports)(uint32_t codepoint));
