/* Keyboard layouts: US and German (QWERTZ). */
#include <kernel.h>
#include <input.h>

int keyboard_layout = LAYOUT_DE;

/* index = key code, 0 = no character */
static const uint16_t us_normal[KEY_MAX] = {
    [KEY_1] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    [KEY_TAB] = '\t',
    [KEY_Q] = 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    [KEY_A] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    [KEY_BACKSLASH] = '\\',
    [KEY_Z] = 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    [KEY_KPASTERISK] = '*', [KEY_SPACE] = ' ',
    [KEY_KP7] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    [KEY_102ND] = '\\', [KEY_KPSLASH] = '/',
};

static const uint16_t us_shift[KEY_MAX] = {
    [KEY_1] = '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',
    [KEY_TAB] = '\t',
    [KEY_Q] = 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',
    [KEY_A] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    [KEY_BACKSLASH] = '|',
    [KEY_Z] = 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    [KEY_KPASTERISK] = '*', [KEY_SPACE] = ' ',
    [KEY_102ND] = '|', [KEY_KPSLASH] = '/',
};

static const uint16_t de_normal[KEY_MAX] = {
    [KEY_1] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xDF /* ß */, 0xB4 /* ´ */,
    [KEY_TAB] = '\t',
    [KEY_Q] = 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', 0xFC /* ü */, '+',
    [KEY_A] = 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xF6 /* ö */, 0xE4 /* ä */, '^',
    [KEY_BACKSLASH] = '#',
    [KEY_Z] = 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-',
    [KEY_KPASTERISK] = '*', [KEY_SPACE] = ' ',
    [KEY_KP7] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', ',',
    [KEY_102ND] = '<', [KEY_KPSLASH] = '/',
};

static const uint16_t de_shift[KEY_MAX] = {
    [KEY_1] = '!', '"', 0xA7 /* § */, '$', '%', '&', '/', '(', ')', '=', '?', '`',
    [KEY_TAB] = '\t',
    [KEY_Q] = 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', 0xDC /* Ü */, '*',
    [KEY_A] = 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xD6 /* Ö */, 0xC4 /* Ä */, 0xB0 /* ° */,
    [KEY_BACKSLASH] = '\'',
    [KEY_Z] = 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_',
    [KEY_KPASTERISK] = '*', [KEY_SPACE] = ' ',
    [KEY_102ND] = '>', [KEY_KPSLASH] = '/',
};

static const uint16_t de_altgr[KEY_MAX] = {
    [KEY_2] = 0xB2 /* ² */, [KEY_3] = 0xB3 /* ³ */,
    [KEY_7] = '{', [KEY_8] = '[', [KEY_9] = ']', [KEY_0] = '}', [KEY_MINUS] = '\\',
    [KEY_Q] = '@', [KEY_E] = 0x20AC /* € */, [KEY_RBRACE] = '~',
    [KEY_M] = 0xB5 /* µ */, [KEY_102ND] = '|',
};

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == 0xE4 || c == 0xF6 || c == 0xFC || c == 0xC4 || c == 0xD6 || c == 0xDC;
}

uint32_t keymap_translate(int key, int mods)
{
    if (key <= 0 || key >= KEY_MAX) return 0;
    if (key == KEY_ENTER || key == KEY_KPENTER) return '\n';
    if (key == KEY_BACKSPACE) return '\b';
    bool de = keyboard_layout == LAYOUT_DE;
    bool shift = mods & MOD_SHIFT;
    if (de && (mods & MOD_ALTGR)) return de_altgr[key];
    uint32_t base = de ? de_normal[key] : us_normal[key];
    if ((mods & MOD_CAPS) && is_letter(base)) shift = !shift;
    if (shift) {
        uint32_t s = de ? de_shift[key] : us_shift[key];
        return s ? s : base;
    }
    return base;
}

const char *keymap_name(int layout)
{
    return layout == LAYOUT_DE ? "Deutsch (QWERTZ)" : "English (US)";
}
