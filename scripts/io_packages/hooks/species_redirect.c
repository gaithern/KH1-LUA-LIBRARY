#include "kh1_native.h"

/* Species-table redirect behind modules/spawn_enemy.lua. The control block is
   shared with Lua, which registers buffer rows and polls the load-complete fields. */

#define CTRL_KEY  "kh1_species_redirect_v2"
#define CTRL_SIZE 0x4000
#define MAGIC     0x52454432  /* 'RED2' */

typedef struct Row {
    uint64_t buf_base;
    uint64_t buf_end;
    uint64_t motion;
    uint32_t active;
    uint32_t tag;
    uint8_t  pad[0x20];
} Row;

#define MAX_ROWS ((CTRL_SIZE - 0x100) / sizeof(Row))

typedef struct Ctrl {
    uint32_t magic;
    uint32_t version;
    uint64_t reverse_blob_hook;
    uint64_t load_complete;      /* Lua passes this to the game as the file-load callback */
    uint64_t blob_base;
    uint64_t unused20;
    uint32_t reg_count;
    uint32_t unused2c;
    volatile uint32_t done_flag;
    volatile uint32_t done_size;
    volatile uint64_t done_dest;
    uint8_t  pad40[0xC0];
    Row      rows[MAX_ROWS];
} Ctrl;

typedef uint64_t (*ReverseBlobFn)(uint64_t blob);

static Ctrl* ctrl;
static ReverseBlobFn orig_reverse_blob;

/* For a blob inside one of our rows, hand back that row's motion pointer
   instead of looking it up in the species table. */
static uint64_t reverse_blob(uint64_t blob) {
    uint32_t count = ctrl->reg_count < MAX_ROWS ? ctrl->reg_count : MAX_ROWS;
    for (uint32_t i = 0; i < count; ++i) {
        Row* r = &ctrl->rows[i];
        if (r->active && blob >= r->buf_base && blob < r->buf_end) return r->motion;
    }
    return orig_reverse_blob(blob);
}

/* File-load completion callback (ecx = size, r8 = dest). The flag goes last; Lua polls it. */
static void load_complete(uint32_t size, uint64_t unused, uint64_t dest) {
    ctrl->done_dest = dest;
    ctrl->done_size = size;
    ctrl->done_flag = 1;
}

int install(void) {
    ctrl = (Ctrl*)kh1_persistent_block(CTRL_KEY, CTRL_SIZE);
    uintptr_t target = kh1_symbol("fnc_reverse_blob_to_slot_ptr");
    uintptr_t blob_base = kh1_symbol("speciesResourceTable");
    if (!ctrl || !target || !blob_base || sizeof(Ctrl) != CTRL_SIZE) return 0;

    if (ctrl->magic != MAGIC) {
        ctrl->blob_base = blob_base;
        ctrl->reverse_blob_hook = (uint64_t)reverse_blob;
        ctrl->load_complete = (uint64_t)load_complete;
        ctrl->reg_count = 0;
        ctrl->done_flag = 0;
    }
    if (!kh1_hook_inline("species_redirect.reverse_blob", target, (void*)reverse_blob, (void**)&orig_reverse_blob))
        return 0;
    ctrl->version = 3;
    ctrl->magic = MAGIC;
    return 1;
}
