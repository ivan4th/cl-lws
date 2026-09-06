/* X keysym -> SDL2 keycode.  Printable Latin-1 keysyms are their own
 * SDL keycode (letters lower-cased, SDL has no upper-case keycodes);
 * keypad digits/operators become the ASCII characters so a viewer's
 * numpad drives the console like its physical keys (the evdev path in
 * cl-lvgl's glue.c makes the same choice); everything else comes from
 * the table below. */

#include "csvnc-private.h"

#define SDLK_SCANCODE(sc) ((int32_t)0x40000000 | (int32_t)(sc))

static const struct {
    uint32_t keysym;
    int32_t sdl;
} keytab[] = {
    { 0xff0d, 13 },                     /* Return -> SDLK_RETURN */
    { 0xff8d, SDLK_SCANCODE(88) },      /* KP_Enter */
    { 0xff1b, 27 },                     /* Escape */
    { 0xff08, 8 },                      /* BackSpace */
    { 0xff09, 9 },                      /* Tab */
    { 0xfe20, 9 },                      /* ISO_Left_Tab (Shift+Tab) */
    { 0xff89, 9 },                      /* KP_Tab */
    { 0xff80, ' ' },                    /* KP_Space */
    { 0xff51, SDLK_SCANCODE(80) },      /* Left */
    { 0xff52, SDLK_SCANCODE(82) },      /* Up */
    { 0xff53, SDLK_SCANCODE(79) },      /* Right */
    { 0xff54, SDLK_SCANCODE(81) },      /* Down */
    { 0xff50, SDLK_SCANCODE(74) },      /* Home */
    { 0xff57, SDLK_SCANCODE(77) },      /* End */
    { 0xff55, SDLK_SCANCODE(75) },      /* Page_Up */
    { 0xff56, SDLK_SCANCODE(78) },      /* Page_Down */
    { 0xff63, SDLK_SCANCODE(73) },      /* Insert */
    { 0xffff, 127 },                    /* Delete -> SDLK_DELETE */
    { 0xff96, SDLK_SCANCODE(80) },      /* KP_Left */
    { 0xff97, SDLK_SCANCODE(82) },      /* KP_Up */
    { 0xff98, SDLK_SCANCODE(79) },      /* KP_Right */
    { 0xff99, SDLK_SCANCODE(81) },      /* KP_Down */
    { 0xff95, SDLK_SCANCODE(74) },      /* KP_Home */
    { 0xff9c, SDLK_SCANCODE(77) },      /* KP_End */
    { 0xff9a, SDLK_SCANCODE(75) },      /* KP_Page_Up */
    { 0xff9b, SDLK_SCANCODE(78) },      /* KP_Page_Down */
    { 0xff9e, SDLK_SCANCODE(73) },      /* KP_Insert */
    { 0xff9f, 127 },                    /* KP_Delete */
    { 0xffab, '+' },                    /* KP_Add */
    { 0xffad, '-' },                    /* KP_Subtract */
    { 0xffaa, '*' },                    /* KP_Multiply */
    { 0xffaf, '/' },                    /* KP_Divide */
    { 0xffae, '.' },                    /* KP_Decimal */
    { 0xffac, ',' },                    /* KP_Separator */
    { 0xffbd, '=' },                    /* KP_Equal */
    { 0xffe1, SDLK_SCANCODE(225) },     /* Shift_L */
    { 0xffe2, SDLK_SCANCODE(229) },     /* Shift_R */
    { 0xffe3, SDLK_SCANCODE(224) },     /* Control_L */
    { 0xffe4, SDLK_SCANCODE(228) },     /* Control_R */
    { 0xffe9, SDLK_SCANCODE(226) },     /* Alt_L */
    { 0xffea, SDLK_SCANCODE(230) },     /* Alt_R */
    { 0xffe7, SDLK_SCANCODE(226) },     /* Meta_L (as Alt) */
    { 0xffe8, SDLK_SCANCODE(230) },     /* Meta_R (as Alt) */
    { 0xffeb, SDLK_SCANCODE(227) },     /* Super_L -> LGUI */
    { 0xffec, SDLK_SCANCODE(231) },     /* Super_R -> RGUI */
    { 0xffe5, SDLK_SCANCODE(57) },      /* Caps_Lock */
    { 0xff7f, SDLK_SCANCODE(83) },      /* Num_Lock -> NUMLOCKCLEAR */
    { 0xff14, SDLK_SCANCODE(71) },      /* Scroll_Lock */
    { 0xff61, SDLK_SCANCODE(70) },      /* Print -> PRINTSCREEN */
    { 0xff13, SDLK_SCANCODE(72) },      /* Pause */
    { 0xff67, SDLK_SCANCODE(101) },     /* Menu -> APPLICATION */
};

int32_t csvnc_keysym_to_sdl(uint32_t keysym)
{
    size_t i;

    if (keysym >= 0x20 && keysym <= 0x7e) {
        if (keysym >= 'A' && keysym <= 'Z')
            return (int32_t)(keysym + ('a' - 'A'));
        return (int32_t)keysym;
    }
    if (keysym >= 0xa0 && keysym <= 0xff)
        return (int32_t)keysym;
    if (keysym >= 0xffbe && keysym <= 0xffc9)          /* F1..F12 */
        return SDLK_SCANCODE(58 + (keysym - 0xffbe));
    if (keysym >= 0xffb0 && keysym <= 0xffb9)          /* KP_0..KP_9 */
        return (int32_t)('0' + (keysym - 0xffb0));
    for (i = 0; i < sizeof(keytab) / sizeof(keytab[0]); i++)
        if (keytab[i].keysym == keysym)
            return keytab[i].sdl;
    return 0;
}
