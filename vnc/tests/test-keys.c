/* X keysym -> SDL keycode table. */

#include "../csvnc-private.h"
#include "test-util.h"

#define SC(n) (0x40000000 | (n))

static void test_printable(void)
{
    uint32_t k;

    for (k = 0x20; k <= 0x7e; k++) {
        int32_t expect = (k >= 'A' && k <= 'Z') ? (int32_t)(k + 32) : (int32_t)k;

        TCHECK_EQ(csvnc_keysym_to_sdl(k), expect);
    }
    for (k = 0xa0; k <= 0xff; k++)
        TCHECK_EQ(csvnc_keysym_to_sdl(k), (int32_t)k);
    TCHECK_EQ(csvnc_keysym_to_sdl('1'), '1');
    TCHECK_EQ(csvnc_keysym_to_sdl('a'), 'a');
    TCHECK_EQ(csvnc_keysym_to_sdl('A'), 'a');
    TCHECK_EQ(csvnc_keysym_to_sdl(' '), ' ');
    /* control range and 0x7f-0x9f are not printable */
    for (k = 0; k < 0x20; k++)
        TCHECK_EQ(csvnc_keysym_to_sdl(k), 0);
    for (k = 0x7f; k < 0xa0; k++)
        TCHECK_EQ(csvnc_keysym_to_sdl(k), 0);
}

static void test_named(void)
{
    /* the console vocabulary (glue.c clvgl_key_* constants) */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff0d), 13);            /* Return */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff8d), SC(88));        /* KP_Enter */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff1b), 27);            /* Escape */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff51), SC(80));        /* Left */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff52), SC(82));        /* Up */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff53), SC(79));        /* Right */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff54), SC(81));        /* Down */
    /* the rest of the table */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff08), 8);             /* BackSpace */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff09), 9);             /* Tab */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xfe20), 9);             /* ISO_Left_Tab */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff50), SC(74));        /* Home */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff57), SC(77));        /* End */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff55), SC(75));        /* Page_Up */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff56), SC(78));        /* Page_Down */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff63), SC(73));        /* Insert */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffff), 127);           /* Delete */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffe1), SC(225));       /* Shift_L */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffe2), SC(229));       /* Shift_R */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffe3), SC(224));       /* Control_L */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffe4), SC(228));       /* Control_R */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffe9), SC(226));       /* Alt_L */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffea), SC(230));       /* Alt_R */
}

static void test_function_keys(void)
{
    uint32_t k;

    for (k = 0; k < 12; k++)
        TCHECK_EQ(csvnc_keysym_to_sdl(0xffbe + k), SC(58 + k));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffbe), SC(58));        /* F1 */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffc9), SC(69));        /* F12 */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffca), 0);             /* F13: unmapped */
}

static void test_keypad(void)
{
    uint32_t k;

    /* keypad digits drive the console like its physical keys */
    for (k = 0; k < 10; k++)
        TCHECK_EQ(csvnc_keysym_to_sdl(0xffb0 + k), (int32_t)('0' + k));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffab), '+');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffad), '-');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffaa), '*');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffaf), '/');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffae), '.');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffac), ',');
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffbd), '=');
    /* keypad navigation = plain navigation */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff96), SC(80));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff97), SC(82));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff98), SC(79));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff99), SC(81));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff95), SC(74));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9c), SC(77));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9a), SC(75));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9b), SC(78));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9e), SC(73));
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9f), 127);
}

static void test_unmapped(void)
{
    TCHECK_EQ(csvnc_keysym_to_sdl(0), 0);
    TCHECK_EQ(csvnc_keysym_to_sdl(0x100), 0);              /* Latin-2 */
    TCHECK_EQ(csvnc_keysym_to_sdl(0x6c1), 0);              /* Cyrillic_a */
    TCHECK_EQ(csvnc_keysym_to_sdl(0x1000000 + 0x263a), 0); /* Unicode keysym */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xff9d), 0);             /* KP_Begin */
    TCHECK_EQ(csvnc_keysym_to_sdl(0xffffffffu), 0);
}

TEST_MAIN(test_printable, test_named, test_function_keys, test_keypad,
          test_unmapped)
