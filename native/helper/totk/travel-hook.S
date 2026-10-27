// TOTK 1.4.2 travel bridge (format-2 load plan, see tools/totk/build_helper_plan.py).
//
// The plan patches the UI thread's per-frame `bl main+0x387070` at main+0x386F30 into a `bl`
// to totk_ui_hook. Each frame the hook bumps the heartbeat and, when the module has staged a
// new request, calls the game's own warp request with a start-position name, exactly as the
// map screen's Travel button does, then tail-calls the original routine with the caller's
// registers intact.
//
// Mailbox (offsets shared with native/modules/totk_travel_mailbox.h):
//   0x000 u32 magic        written by the hook every frame ('TKTV')
//   0x004 u32 heartbeat    +1 per frame
//   0x008 u32 req_seq      the module bumps it (release) after filling the request
//   0x00C u32 req_op       1 travel, 2 equip, 3 unequip, 4 add item, 5 eat
//   0x010 u32 req_epoch    must equal the plan's epoch word at 0xFFC
//   0x014 u32 arg0         pouch category (0 weapon, 1 bow, 2 arrow, 3 shield, 4 armor,
//                          5 material, 6 food)
//   0x018 u32 arg1         equip/unequip/eat: Pouch.<Cat>.Content index; add: count
//   0x060 u32 result_seq   = req_seq once handled (release)
//   0x064 u32 result_code  0 ok, 1 refused by the game, 2 bad op, 3 stale epoch, 4 no receiver
//   0x100 char name[256]   travel: "StartPos_Tower02"; add: the item's actor name
//   0xFFC u32 epoch        the runtime's epoch word
//
// warp(x0 = receiver, x1 = &{u64 ptr, u32 cap, char buf[0x100] at +0xC}) returns non-zero
// when it accepted the name. receiver = [[[[main+0x3ACB230]+0x1E8]+0x58]+0x100].
//
// equip(x0 = pouch, w1 = category, w2 = slot, w3 = -1) and
// unequip(x0 = pouch, w1 = category, w2 = slot, w3 = -1, w4 = 0, w5 = 1) are the pause menu's
// own Equip and Remove (live model swap included); both return non-zero when done.
// add(x0 = pouch, x1 = &name struct, x2 = &main+0x360C068, w3 = category, w4 = count, w5 = 1,
// w6 = 9, w7 = 0, stack {0, -1 x5, 0, (u8)1}) is the event system's add-to-pouch (default
// stats); eat(x0 = pouch, w1 = category, w2 = slot) is the menu's Eat. pouch = [main+0x3A6F678].

.text
.global totk_ui_hook
totk_ui_hook:
    stp x29, x30, [sp, #-0x180]!
    mov x29, sp
    stp x0, x1, [sp, #0x10]
    stp x2, x3, [sp, #0x20]
    stp x4, x5, [sp, #0x30]
    stp x6, x7, [sp, #0x40]
    stp x19, x20, [sp, #0x50]
.global mailbox_adrp, mailbox_add
mailbox_adrp:
    adrp x19, bridge_mailbox
mailbox_add:
    add x19, x19, :lo12:bridge_mailbox
    mov w9, #0x5654          // 'TV'
    movk w9, #0x4b54, lsl #16 // 'TK'
    str w9, [x19]
    ldr w9, [x19, #4]
    add w9, w9, #1
    str w9, [x19, #4]

    add x9, x19, #8
    ldar w20, [x9]           // req_seq
    ldr w9, [x19, #0x60]
    cmp w20, w9
    b.eq done                // nothing new

    ldr w9, [x19, #0x10]
    add x10, x19, #4092
    ldar w10, [x10]
    mov w0, #3
    cbz w10, answer
    cmp w9, w10
    b.ne answer              // staged before a pause/reload: drop it

    ldr w9, [x19, #0xC]
    cmp w9, #1
    b.eq travel
    cmp w9, #2
    b.eq equip
    cmp w9, #3
    b.eq equip
    cmp w9, #4
    b.eq add_item
    cmp w9, #5
    b.eq eat
    mov w0, #2               // unknown op
    b answer

equip:
    mov w0, #4
    adrp x10, pouch_root
    ldr x10, [x10, :lo12:pouch_root]
    cbz x10, answer
    mov x0, x10
    ldr w1, [x19, #0x14]
    ldr w2, [x19, #0x18]
    mov w3, #-1
    cmp w9, #3
    b.eq unequip
    bl native_equip
    b called
unequip:
    mov w4, #0
    mov w5, #1
    bl native_unequip
    b called

add_item:
    mov w0, #4
    adrp x10, pouch_root
    ldr x10, [x10, :lo12:pouch_root]
    cbz x10, answer
    bl build_name
    sub sp, sp, #0x40        // stack arguments
    stp xzr, xzr, [sp]
    mov x9, #-1
    stp x9, x9, [sp, #0x08]
    stp x9, x9, [sp, #0x18]
    str x9, [sp, #0x28]
    str xzr, [sp, #0x30]
    mov x9, #1
    str x9, [sp, #0x38]
    mov x0, x10
    add x1, sp, #0xA0        // the name struct (sp+0x60 before the sub)
    adrp x2, add_defaults
    add x2, x2, :lo12:add_defaults
    ldr w3, [x19, #0x14]
    ldr w4, [x19, #0x18]
    mov w5, #1
    mov w6, #9
    mov w7, #0
    bl native_add
    add sp, sp, #0x40
    b called

eat:
    mov w0, #4
    adrp x10, pouch_root
    ldr x10, [x10, :lo12:pouch_root]
    cbz x10, answer
    mov x0, x10
    ldr w1, [x19, #0x14]
    ldr w2, [x19, #0x18]
    bl native_eat
    b called

travel:
    // receiver chain, every hop checked
    mov w0, #4
    adrp x9, warp_root
    ldr x9, [x9, :lo12:warp_root]
    cbz x9, answer
    ldr x9, [x9, #0x1E8]
    cbz x9, answer
    ldr x9, [x9, #0x58]
    cbz x9, answer
    ldr x9, [x9, #0x100]
    cbz x9, answer

    bl build_name
    add x1, sp, #0x60
    mov x0, x9
    bl native_warp
    cmp x0, #0
    cset w0, eq              // 0 accepted, 1 refused
    b answer

called:
    tst w0, #0xFF            // bool return
    cset w0, eq              // 0 done, 1 refused

answer:
    str w0, [x19, #0x64]
    add x9, x19, #0x60
    stlr w20, [x9]

done:
    ldp x0, x1, [sp, #0x10]
    ldp x2, x3, [sp, #0x20]
    ldp x4, x5, [sp, #0x30]
    ldp x6, x7, [sp, #0x40]
    ldp x19, x20, [sp, #0x50]
    ldp x29, x30, [sp], #0x180
    b native_ui_update

// Leaf: build {ptr, cap 0x100, buf at +0xC} at the hook frame's sp+0x60 from mailbox+0x100.
// Uses x11-x15 only; returns with x1 = the struct.
build_name:
    add x1, sp, #0x60
    add x15, x1, #0xC
    str x15, [x1]
    mov w11, #0x100
    str w11, [x1, #8]
    add x12, x19, #0x100
    mov x13, #0
1:
    ldrb w14, [x12, x13]
    strb w14, [x15, x13]
    cbz w14, 2f
    add x13, x13, #1
    cmp x13, #0xFF
    b.lo 1b
    strb wzr, [x15, x13]
2:
    ret
