// Host test: credentials encoded by tools/obfuscate_keys.py decode back exactly.
// Built against a header generated from main/bp_baidu_keys.example.h.
#include "bp_keys.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    char buf[BP_KEY_BUF];
    assert(bp_key_get(BP_KEY_APPKEY, buf, sizeof(buf)) == strlen("REPLACE_WITH_YOUR_BAIDU_APP_KEY"));
    assert(!strcmp(buf, "REPLACE_WITH_YOUR_BAIDU_APP_KEY"));
    assert(bp_key_get(BP_KEY_SECRET, buf, sizeof(buf)) == strlen("REPLACE_WITH_YOUR_BAIDU_APP_SECRET"));
    assert(!strcmp(buf, "REPLACE_WITH_YOUR_BAIDU_APP_SECRET"));
    assert(bp_keys_placeholder());
    // Too-small buffer: no partial secret, empty string.
    char small[8] = "xxxxxxx";
    assert(bp_key_get(BP_KEY_SECRET, small, sizeof(small)) == 0 && small[0] == 0);
    bp_key_wipe(buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) assert(buf[i] == 0);
    puts("test_bp_keys: PASS");
    return 0;
}
