#include "bp_keys.h"

#include <stdint.h>
#include <string.h>

#include "bp_keys_obf.h"   // 构建时生成,见 tools/obfuscate_keys.py

// 数组为 volatile:逐字节读取,编译器无法在编译期算出明文常量。
static size_t decode(const volatile uint8_t *data, const volatile uint8_t *mask, size_t len,
                     char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    if (len + 1 > cap) {
        out[0] = 0;
        return 0;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = (char)(data[i] ^ mask[i] ^ (uint8_t)(i * 0x5B + 0x3D));
    out[len] = 0;
    return len;
}

size_t bp_key_get(bp_key_id_t id, char *out, size_t cap) {
    switch (id) {
        case BP_KEY_APPKEY:
            return decode(bp_key_appkey_data, bp_key_appkey_mask, BP_KEY_APPKEY_LEN, out, cap);
        case BP_KEY_SECRET:
            return decode(bp_key_secret_data, bp_key_secret_mask, BP_KEY_SECRET_LEN, out, cap);
    }
    if (out && cap) out[0] = 0;
    return 0;
}

void bp_key_wipe(void *buf, size_t len) {
    volatile uint8_t *p = buf;
    while (len--) *p++ = 0;
}

bool bp_keys_placeholder(void) { return BP_KEYS_PLACEHOLDER != 0; }
