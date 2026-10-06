#include "kh1_native.h"

/* Custom-text hooks behind modules/prize_popup.lua. The state block is shared
   with Lua, which writes the text and raises the flag before showing the popup. */

#define STATE_KEY  "kh1_popup_state_v1"
#define STATE_SIZE 0x220
#define OFF_FLAG   0x00
#define OFF_PREV   0x04
#define OFF_BUF    0x20

static uint8_t* state;
static const int32_t* popup_state;

/* After mov rdi, rax, before the text call: point the text at our buffer while the flag is set. */
static void popup_text(KH1Context* c) {
    if (state[OFF_FLAG] == 1) c->rdi = (uint64_t)(state + OFF_BUF);
}

/* Popup tick entry: clear the flag once the popup state goes from open back to 0. */
static void popup_tick(KH1Context* c) {
    int32_t now = *popup_state;
    int32_t* prev = (int32_t*)(state + OFF_PREV);
    if (*prev != 0 && now == 0) state[OFF_FLAG] = 0;
    *prev = now;
}

static const uint8_t mov_rdi_rax[] = { 0x48, 0x8B, 0xF8, 0xE8 };
static const uint8_t mov_rdi_rax_alt[] = { 0x48, 0x89, 0xC7, 0xE8 };
static const uint8_t tick_entry[] = { 0x48, 0x83, 0xEC, 0x28, 0x33, 0xD2 };

int install(void) {
    state = (uint8_t*)kh1_persistent_block(STATE_KEY, STATE_SIZE);
    popup_state = (const int32_t*)kh1_symbol("g_item_popup_state");
    uintptr_t text = kh1_symbol("fnc_item_popup_text_hook");
    uintptr_t tick = kh1_symbol("fnc_item_popup_tick");
    if (!state || !popup_state || !text || !tick) return 0;
    if (memcmp((const void*)text, mov_rdi_rax, sizeof(mov_rdi_rax)) != 0
        && memcmp((const void*)text, mov_rdi_rax_alt, sizeof(mov_rdi_rax_alt)) != 0) return 0;
    if (memcmp((const void*)tick, tick_entry, sizeof(tick_entry)) != 0) return 0;

    return kh1_hook_mid("prize_popup.text", text + 3, popup_text)
        && kh1_hook_mid("prize_popup.tick", tick, popup_tick);
}
