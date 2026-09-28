#include "ssid_text.h"

#include <stdio.h>
#include <string.h>

static bool append(char *out, size_t out_len, size_t *used, const char *part, size_t count) {
    if (*used + count >= out_len) return false;
    memcpy(out + *used, part, count);
    *used += count;
    out[*used] = 0;
    return true;
}

void ssid_format(const uint8_t *ssid, size_t len, char *out, size_t out_len,
                 bool (*supports)(uint32_t codepoint)) {
    if (!out_len) return;
    out[0] = 0;
    size_t used = 0;
    for (size_t at = 0; at < len;) {
        uint8_t lead = ssid[at];
        size_t width = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
                       lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        uint32_t codepoint = lead & (width == 1 ? 0x7f : width == 2 ? 0x1f : width == 3 ? 0x0f : 0x07);
        bool valid = width && at + width <= len;
        for (size_t i = 1; valid && i < width; ++i) {
            valid = (ssid[at + i] & 0xc0) == 0x80;
            codepoint = (codepoint << 6) | (ssid[at + i] & 0x3f);
        }
        if (valid && ((width == 2 && codepoint < 0x80) ||
                      (width == 3 && (codepoint < 0x800 || (codepoint >= 0xd800 && codepoint <= 0xdfff))) ||
                      (width == 4 && (codepoint < 0x10000 || codepoint > 0x10ffff)))) valid = false;
        if (!valid) width = 1;
        if (valid && supports(codepoint)) {
            if (!append(out, out_len, &used, (const char *)ssid + at, width)) break;
        } else {
            char hex[11] = "<";
            for (size_t i = 0; i < width; ++i) {
                snprintf(hex + 1 + i * 2, sizeof hex - 1 - i * 2, "%02X", ssid[at + i]);
            }
            hex[1 + width * 2] = '>';
            if (!append(out, out_len, &used, hex, 2 + width * 2)) return;
        }
        at += width;
    }
}
