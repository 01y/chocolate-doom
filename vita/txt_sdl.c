//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
// Text mode emulation in SDL, Vita version
//

#include "SDL.h"
#include <vita2d.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomkeys.h"

#include "txt_main.h"
#include "txt_sdl.h"
#include "txt_utf8.h"

#define VITA_SCR_W 960
#define VITA_SCR_H 544

#if defined(_MSC_VER) && !defined(__cplusplus)
#define inline __inline
#endif

typedef struct
{
    const char *name;
    const uint8_t *data;
    unsigned int w;
    unsigned int h;
} txt_font_t;

// Fonts:

#include "fonts/small.h"
#include "fonts/normal.h"
#include "fonts/large.h"
#include "fonts/codepage.h"

// Time between character blinks in ms

#define BLINK_PERIOD 250

// HACK: the SDL2 port can't handle being reinitialized, so we hijack
//       the window and renderer from i_video.c
extern SDL_Window *sdl_window;
extern SDL_Renderer *sdl_renderer;

SDL_Window *TXT_SDLWindow;
static SDL_Surface *screenbuffer;
static vita2d_texture *screentex;
static uint8_t *screentex_datap;
static unsigned char *screendata;
static SDL_Renderer *renderer;
static SDL_Rect target_rect;
static SDL_Joystick *joystick;

static float target_sx = 1.f;
static float target_sy = 1.f;

// Current input mode.
static txt_input_mode_t input_mode = TXT_INPUT_NORMAL;

// Dimensions of the screen image in screen coordinates (not pixels); this
// is the value that was passed to SDL_CreateWindow().
static int screen_image_w, screen_image_h;

static TxtSDLEventCallbackFunc event_callback;
static void *event_callback_data;

// Font we are using:
static const txt_font_t *font;

// Dummy "font" that means to try highdpi rendering, or fallback to
// normal_font otherwise.
static const txt_font_t highdpi_font = { "normal-highdpi", NULL, 8, 16 };

// Mapping from SDL keyboard scancode to internal key code.
static const int scancode_translate_table[] = SCANCODE_TO_KEYS_ARRAY;

// String names of keys. This is a fallback; we usually use the SDL API.
static const struct {
    int key;
    const char *name;
} key_names[] = KEY_NAMES_ARRAY;

// Unicode key mapping; see codepage.h.
static const short code_page_to_unicode[] = CODE_PAGE_TO_UNICODE;

static SDL_Color ega_colors[] =
{
    {0x00, 0x00, 0x00, 0xff},          // 0: Black
    {0x00, 0x00, 0xa8, 0xff},          // 1: Blue
    {0x00, 0xa8, 0x00, 0xff},          // 2: Green
    {0x00, 0xa8, 0xa8, 0xff},          // 3: Cyan
    {0xa8, 0x00, 0x00, 0xff},          // 4: Red
    {0xa8, 0x00, 0xa8, 0xff},          // 5: Magenta
    {0xa8, 0x54, 0x00, 0xff},          // 6: Brown
    {0xa8, 0xa8, 0xa8, 0xff},          // 7: Grey
    {0x54, 0x54, 0x54, 0xff},          // 8: Dark grey
    {0x54, 0x54, 0xfe, 0xff},          // 9: Bright blue
    {0x54, 0xfe, 0x54, 0xff},          // 10: Bright green
    {0x54, 0xfe, 0xfe, 0xff},          // 11: Bright cyan
    {0xfe, 0x54, 0x54, 0xff},          // 12: Bright red
    {0xfe, 0x54, 0xfe, 0xff},          // 13: Bright magenta
    {0xfe, 0xfe, 0x54, 0xff},          // 14: Yellow
    {0xfe, 0xfe, 0xfe, 0xff},          // 15: Bright white
};

// This is defined in i_video.c and provides a centralized way of
// creating a single window and renderer pair for both text mode and
// graphics.

extern void I_VitaInitSDLWindow(void);

//
// Initialize text mode screen
//
// Returns 1 if successful, 0 if an error occurred
//

//01y

void TXT_SetColor(txt_color_t color, int r, int g, int b)
{
    SDL_Color c = {r, g, b, 0xff};

    SDL_LockSurface(screenbuffer);
    SDL_SetPaletteColors(screenbuffer->format->palette, &c, color, 1);
    SDL_UnlockSurface(screenbuffer);
}


int TXT_Init(void)
{
    font = &normal_font;

    screen_image_w = TXT_SCREEN_W * font->w;
    screen_image_h = TXT_SCREEN_H * font->h;

    if (!sdl_window || !sdl_renderer)
    {
        I_VitaInitSDLWindow();
    }

    TXT_SDLWindow = sdl_window;
    renderer = sdl_renderer;

    if (TXT_SDLWindow == NULL)
        return 0;

    // Instead, we draw everything into an intermediate 8-bit surface
    // the same dimensions as the screen. SDL then takes care of all the
    // 8->32 bit (or whatever depth) color conversions for us.
    screenbuffer = SDL_CreateRGBSurface(0,
                                        TXT_SCREEN_W * font->w,
                                        TXT_SCREEN_H * font->h,
                                        8, 0, 0, 0, 0);

    if (!screentex)
    {
        screentex = vita2d_create_empty_texture_format(
            TXT_SCREEN_W * font->w, TXT_SCREEN_H * font->h,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8
        );
        screentex_datap = vita2d_texture_get_datap(screentex);
    }

    vita2d_texture_set_filters(screentex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);

    SDL_SetPaletteColors(screenbuffer->format->palette, ega_colors, 0, 16);

    screendata = malloc(TXT_SCREEN_W * TXT_SCREEN_H * 2);
    memset(screendata, 0, TXT_SCREEN_W * TXT_SCREEN_H * 2);

    // center and scale the text screen
    target_rect.h = VITA_SCR_H;
    target_rect.w = (VITA_SCR_H * (float) screenbuffer->w) / (float) screenbuffer->h;
    target_rect.y = 0;
    target_rect.x = (VITA_SCR_W - target_rect.w) / 2;
    target_sx = (float) target_rect.w / (float) screenbuffer->w;
    target_sy = (float) target_rect.h / (float) screenbuffer->h;

    if (!SDL_WasInit(SDL_INIT_JOYSTICK))
    {
        SDL_Init(SDL_INIT_JOYSTICK);
    }

    joystick = SDL_JoystickOpen(0);
    SDL_JoystickEventState(SDL_ENABLE);

    return 1;
}

void TXT_Shutdown(void)
{
    vita2d_wait_rendering_done();
    free(screendata);
    screendata = NULL;
    SDL_FreeSurface(screenbuffer);
    screenbuffer = NULL;
    SDL_JoystickClose(joystick);
    joystick = NULL;
    // don't deinit SDL here, makes shit crash
}

unsigned char *TXT_GetScreenData(void)
{
    return screendata;
}

static inline void UpdateCharacter(int x, int y)
{
    unsigned char character;
    const uint8_t *p;
    unsigned char *s, *s1;
    unsigned int bit;
    int bg, fg;
    unsigned int x1, y1;

    p = &screendata[(y * TXT_SCREEN_W + x) * 2];
    character = p[0];

    fg = p[1] & 0xf;
    bg = (p[1] >> 4) & 0xf;

    if (bg & 0x8)
    {
        // blinking

        bg &= ~0x8;

        if (((SDL_GetTicks() / BLINK_PERIOD) % 2) == 0)
        {
            fg = bg;
        }
    }

    // How many bytes per line?
    p = &font->data[(character * font->w * font->h) / 8];
    bit = 0;

    s = ((unsigned char *) screenbuffer->pixels)
      + (y * font->h * screenbuffer->pitch)
      + (x * font->w);

    for (y1=0; y1<font->h; ++y1)
    {
        s1 = s;

        for (x1=0; x1<font->w; ++x1)
        {
            if (*p & (1 << bit))
            {
                *s1++ = fg;
            }
            else
            {
                *s1++ = bg;
            }

            ++bit;
            if (bit == 8)
            {
                ++p;
                bit = 0;
            }
        }

        s += screenbuffer->pitch;
    }
}

static int LimitToRange(int val, int min, int max)
{
    if (val < min)
    {
        return min;
    }
    else if (val > max)
    {
        return max;
    }
    else
    {
        return val;
    }
}

// TODO: accelerate this
static inline void BlitBuffer(void)
{
    SDL_Color c;
    uint8_t *dst;
    const uint8_t *src, *end;

    src = screenbuffer->pixels;
    dst = screentex_datap;
    end = src + screenbuffer->pitch * screenbuffer->h;
    for (; src < end; ++src)
    {
        c = ega_colors[*src];
        // ABGR
        *(dst++) = c.r;
        *(dst++) = c.g;
        *(dst++) = c.b;
        *(dst++) = 255;
    }
}

void TXT_UpdateScreenArea(int x, int y, int w, int h)
{
    int x1, y1;
    int x_end;
    int y_end;

    x_end = LimitToRange(x + w, 0, TXT_SCREEN_W);
    y_end = LimitToRange(y + h, 0, TXT_SCREEN_H);
    x = LimitToRange(x, 0, TXT_SCREEN_W);
    y = LimitToRange(y, 0, TXT_SCREEN_H);

    for (y1=y; y1<y_end; ++y1)
    {
        for (x1=x; x1<x_end; ++x1)
        {
            UpdateCharacter(x1, y1);
        }
    }

    // Have to blit manually here because it seems LowerBlit is bugged on Vita.
    // Blitting is done directly to native texture
    BlitBuffer();

    // Native rendering, bypassing any SDL2 functions that might slow things down
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_texture_scale(
        screentex,
        target_rect.x, target_rect.y,
        target_sx, target_sy
    );
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void TXT_UpdateScreen(void)
{
    TXT_UpdateScreenArea(0, 0, TXT_SCREEN_W, TXT_SCREEN_H);
}

void TXT_GetMousePosition(int *x, int *y)
{
    int window_w, window_h;
    int origin_x, origin_y;

    SDL_GetMouseState(x, y);

    // Translate mouse position from 'pixel' position into character position.
    // We are working here in screen coordinates and not pixels, since this is
    // what SDL_GetWindowSize() returns; we must calculate and subtract the
    // origin position since we center the image within the window.
    SDL_GetWindowSize(TXT_SDLWindow, &window_w, &window_h);
    origin_x = (window_w - screen_image_w) / 2;
    origin_y = (window_h - screen_image_h) / 2;
    *x = ((*x - origin_x) * TXT_SCREEN_W) / screen_image_w;
    *y = ((*y - origin_y) * TXT_SCREEN_H) / screen_image_h;

    if (*x < 0)
    {
        *x = 0;
    }
    else if (*x >= TXT_SCREEN_W)
    {
        *x = TXT_SCREEN_W - 1;
    }
    if (*y < 0)
    {
        *y = 0;
    }
    else if (*y >= TXT_SCREEN_H)
    {
        *y = TXT_SCREEN_H - 1;
    }
}

//
// Translates the SDL key
//

// XXX: duplicate from doomtype.h
#define arrlen(array) (sizeof(array) / sizeof(*array))

static int TranslateScancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
            return KEY_RCTRL;

        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
            return KEY_RSHIFT;

        case SDL_SCANCODE_LALT:
            return KEY_LALT;

        case SDL_SCANCODE_RALT:
            return KEY_RALT;

        default:
            if (scancode < arrlen(scancode_translate_table))
            {
                return scancode_translate_table[scancode];
            }
            else
            {
                return 0;
            }
    }
}

static int TranslateKeysym(const SDL_Keysym *sym)
{
    int translated;

    // We cheat here and make use of TranslateScancode. The range of keys
    // associated with printable characters is pretty contiguous, so if it's
    // inside that range we want the localized version of the key instead.
    translated = TranslateScancode(sym->scancode);

    if (translated >= 0x20 && translated < 0x7f)
    {
        return sym->sym;
    }
    else
    {
        return translated;
    }
}

// Convert an SDL button index to textscreen button index.
//
// Note special cases because 2 == mid in SDL, 3 == mid in textscreen/setup

static int SDLButtonToTXTButton(int button)
{
    switch (button)
    {
        case SDL_BUTTON_LEFT:
            return TXT_MOUSE_LEFT;
        case SDL_BUTTON_RIGHT:
            return TXT_MOUSE_RIGHT;
        case SDL_BUTTON_MIDDLE:
            return TXT_MOUSE_MIDDLE;
        default:
            return TXT_MOUSE_BASE + button - 1;
    }
}

// Convert an SDL wheel motion to a textscreen button index.

static int SDLWheelToTXTButton(const SDL_MouseWheelEvent *wheel)
{
    if (wheel->y <= 0)
    {
        return TXT_MOUSE_SCROLLDOWN;
    }
    else
    {
        return TXT_MOUSE_SCROLLUP;
    }
}

static int MouseHasMoved(void)
{
    static int last_x = 0, last_y = 0;
    int x, y;

    TXT_GetMousePosition(&x, &y);

    if (x != last_x || y != last_y)
    {
        last_x = x; last_y = y;
        return 1;
    }
    else
    {
        return 0;
    }
}

// XXX: duplicate from i_video.c
static void TranslateControllerEvent(SDL_Event *ev)
{
    int btn;
    SDL_Event ev_new;
    int in_prompt;
    static const struct 
    {
        SDL_Keycode sym;
        SDL_Scancode scan;
    } v_keymap[] = 
    {
        { SDLK_LALT, SDL_SCANCODE_LALT },           // Triangle
        { SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE }, // Circle
        { SDLK_RETURN, SDL_SCANCODE_RETURN },       // Cross
        { SDLK_SPACE, SDL_SCANCODE_SPACE },         // Square
        { SDLK_LSHIFT, SDL_SCANCODE_LSHIFT },       // L Trigger
        { SDLK_LCTRL, SDL_SCANCODE_LCTRL },         // R Trigger
        { SDLK_DOWN, SDL_SCANCODE_DOWN },           // D-Down
        { SDLK_LEFT, SDL_SCANCODE_LEFT },           // D-Left
        { SDLK_UP, SDL_SCANCODE_UP },               // D-Up
        { SDLK_RIGHT, SDL_SCANCODE_RIGHT },         // D-Right
        { SDLK_DELETE, SDL_SCANCODE_DELETE },       // Select
        { SDLK_ESCAPE, SDL_SCANCODE_ESCAPE },       // Start
    };
    
    memset(&ev_new, 0, sizeof(SDL_Event));

    btn = ev->jbutton.button;
    in_prompt = 0; // TODO

    if (in_prompt)
    {
        if (btn == 1 || btn == 10)
        {
            ev_new.key.keysym.sym = SDLK_n;
            ev_new.key.keysym.scancode = SDL_SCANCODE_N;
        }
        else if (btn == 2 || btn == 11)
        {
            ev_new.key.keysym.sym = SDLK_y;
            ev_new.key.keysym.scancode = SDL_SCANCODE_Y;
        }
        else
        {
            return;
        }
    }
    else
    {
        if (btn < 0 || btn > 11)
            return;
        ev_new.key.keysym.sym = v_keymap[btn].sym;
        ev_new.key.keysym.scancode = v_keymap[btn].scan;
    }

    if (ev->type == SDL_JOYBUTTONDOWN)
    {
        ev_new.type = ev_new.key.type = SDL_KEYDOWN;
        ev_new.key.state = SDL_PRESSED;
    }
    else if (ev->type == SDL_JOYBUTTONUP)
    {
        ev_new.type = ev_new.key.type = SDL_KEYUP;
        ev_new.key.state = SDL_RELEASED;
    }

    SDL_PushEvent(&ev_new);
}

signed int TXT_GetChar(void)
{
    SDL_Event ev;

    while (SDL_PollEvent(&ev))
    {
        // If there is an event callback, allow it to intercept this
        // event.

        if (event_callback != NULL)
        {
            if (event_callback(&ev, event_callback_data))
            {
                continue;
            }
        }

        // Process the event.

        switch (ev.type)
        {
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button < TXT_MAX_MOUSE_BUTTONS)
                {
                    return SDLButtonToTXTButton(ev.button.button);
                }
                break;

            case SDL_MOUSEWHEEL:
                return SDLWheelToTXTButton(&ev.wheel);

            case SDL_KEYDOWN:
                switch (input_mode)
                {
                    case TXT_INPUT_RAW:
                        return TranslateScancode(ev.key.keysym.scancode);
                    case TXT_INPUT_NORMAL:
                        return TranslateKeysym(&ev.key.keysym);
                    case TXT_INPUT_TEXT:
                        // We ignore key inputs in this mode, except for a
                        // few special cases needed during text input:
                        if (ev.key.keysym.sym == SDLK_ESCAPE
                         || ev.key.keysym.sym == SDLK_BACKSPACE
                         || ev.key.keysym.sym == SDLK_RETURN)
                        {
                            return TranslateKeysym(&ev.key.keysym);
                        }
                        break;
                }
                break;

            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                TranslateControllerEvent(&ev);
                break;

            case SDL_TEXTINPUT:
                if (input_mode == TXT_INPUT_TEXT)
                {
                    // TODO: Support input of more than just the first char?
                    const char *p = ev.text.text;
                    int result = TXT_DecodeUTF8(&p);
                    // 0-127 is ASCII, but we map non-ASCII Unicode chars into
                    // a higher range to avoid conflicts with special keys.
                    return TXT_UNICODE_TO_KEY(result);
                }
                break;

            case SDL_QUIT:
                // Quit = escape
                return 27;

            case SDL_MOUSEMOTION:
                if (MouseHasMoved())
                {
                    return 0;
                }

            default:
                break;
        }
    }

    return -1;
}

int TXT_GetModifierState(txt_modifier_t mod)
{
    SDL_Keymod state;

    state = SDL_GetModState();

    switch (mod)
    {
        case TXT_MOD_SHIFT:
            return (state & KMOD_SHIFT) != 0;
        case TXT_MOD_CTRL:
            return (state & KMOD_CTRL) != 0;
        case TXT_MOD_ALT:
            return (state & KMOD_ALT) != 0;
        default:
            return 0;
    }
}

int TXT_UnicodeCharacter(unsigned int c)
{
    unsigned int i;

    // Check the code page mapping to see if this character maps
    // to anything.

    for (i = 0; i < arrlen(code_page_to_unicode); ++i)
    {
        if (code_page_to_unicode[i] == c)
        {
            return i;
        }
    }

    return -1;
}

// Returns true if the given UTF8 key name is printable to the screen.
static int PrintableName(const char *s)
{
    const char *p;
    unsigned int c;

    p = s;
    while (*p != '\0')
    {
        c = TXT_DecodeUTF8(&p);
        if (TXT_UnicodeCharacter(c) < 0)
        {
            return 0;
        }
    }

    return 1;
}

static const char *NameForKey(int key)
{
    const char *result;
    int i;

    // Overrides to match Vita button names.
    switch (key)
    {
        case KEY_ESCAPE:    return "START";
        case KEY_ENTER:     return "CROSS";
        case ' ':           return "SQUARE";
        case KEY_BACKSPACE: return "CIRCLE";
        case KEY_LALT:      return "TRIANGLE";
        default:
            break;
    }

    // This key presumably maps to a scan code that is listed in the
    // translation table. Find which mapping and once we have a scancode,
    // we can convert it into a virtual key, then a string via SDL.
    for (i = 0; i < arrlen(scancode_translate_table); ++i)
    {
        if (scancode_translate_table[i] == key)
        {
            result = SDL_GetKeyName(SDL_GetKeyFromScancode(i));
            if (TXT_UTF8_Strlen(result) > 6 || !PrintableName(result))
            {
                break;
            }
            return result;
        }
    }

    // Use US English fallback names, if the localized name is too long,
    // not found in the scancode table, or contains unprintable chars
    // (non-extended ASCII character set):
    for (i = 0; i < arrlen(key_names); ++i)
    {
        if (key_names[i].key == key)
        {
            return key_names[i].name;
        }
    }

    return NULL;
}

void TXT_GetKeyDescription(int key, char *buf, size_t buf_len)
{
    const char *keyname;
    int i;

    keyname = NameForKey(key);

    if (keyname != NULL)
    {
        TXT_StringCopy(buf, keyname, buf_len);

        // Key description should be all-uppercase to match setup.exe.
        for (i = 0; buf[i] != '\0'; ++i)
        {
            buf[i] = toupper(buf[i]);
        }
    }
    else
    {
        TXT_snprintf(buf, buf_len, "??%i", key);
    }
}

// Searches the desktop screen buffer to determine whether there are any
// blinking characters.

int TXT_ScreenHasBlinkingChars(void)
{
    int x, y;
    unsigned char *p;

    // Check all characters in screen buffer

    for (y=0; y<TXT_SCREEN_H; ++y)
    {
        for (x=0; x<TXT_SCREEN_W; ++x) 
        {
            p = &screendata[(y * TXT_SCREEN_W + x) * 2];

            if (p[1] & 0x80)
            {
                // This character is blinking

                return 1;
            }
        }
    }

    // None found

    return 0;
}

// Sleeps until an event is received, the screen needs to be redrawn, 
// or until timeout expires (if timeout != 0)

void TXT_Sleep(int timeout)
{
    unsigned int start_time;

    if (TXT_ScreenHasBlinkingChars())
    {
        int time_to_next_blink;

        time_to_next_blink = BLINK_PERIOD - (SDL_GetTicks() % BLINK_PERIOD);

        // There are blinking characters on the screen, so we 
        // must time out after a while
       
        if (timeout == 0 || timeout > time_to_next_blink)
        {
            // Add one so it is always positive

            timeout = time_to_next_blink + 1;
        }
    }

    if (timeout == 0)
    {
        // We can just wait forever until an event occurs

        SDL_WaitEvent(NULL);
    }
    else
    {
        // Sit in a busy loop until the timeout expires or we have to
        // redraw the blinking screen

        start_time = SDL_GetTicks();

        while (SDL_GetTicks() < start_time + timeout)
        {
            if (SDL_PollEvent(NULL) != 0)
            {
                // Received an event, so stop waiting

                break;
            }

            // Don't hog the CPU

            SDL_Delay(1);
        }
    }
}

void TXT_SetInputMode(txt_input_mode_t mode)
{
    if (mode == TXT_INPUT_TEXT && !SDL_IsTextInputActive())
    {
        SDL_StartTextInput();
    }
    else if (SDL_IsTextInputActive() && mode != TXT_INPUT_TEXT)
    {
        SDL_StopTextInput();
    }

    input_mode = mode;
}

void TXT_SetWindowTitle(const char *title)
{
    SDL_SetWindowTitle(TXT_SDLWindow, title);
}

void TXT_SDL_SetEventCallback(TxtSDLEventCallbackFunc callback, void *user_data)
{
    event_callback = callback;
    event_callback_data = user_data;
}

// Safe string functions.

void TXT_StringCopy(char *dest, const char *src, size_t dest_len)
{
    if (dest_len < 1)
    {
        return;
    }

    dest[dest_len - 1] = '\0';
    strncpy(dest, src, dest_len - 1);
}

void TXT_StringConcat(char *dest, const char *src, size_t dest_len)
{
    size_t offset;

    offset = strlen(dest);
    if (offset > dest_len)
    {
        offset = dest_len;
    }

    TXT_StringCopy(dest + offset, src, dest_len - offset);
}

// On Windows, vsnprintf() is _vsnprintf().
#ifdef _WIN32
#if _MSC_VER < 1400 /* not needed for Visual Studio 2008 */
#define vsnprintf _vsnprintf
#endif
#endif

// Safe, portable vsnprintf().
int TXT_vsnprintf(char *buf, size_t buf_len, const char *s, va_list args)
{
    int result;

    if (buf_len < 1)
    {
        return 0;
    }

    // Windows (and other OSes?) has a vsnprintf() that doesn't always
    // append a trailing \0. So we must do it, and write into a buffer
    // that is one byte shorter; otherwise this function is unsafe.
    result = vsnprintf(buf, buf_len, s, args);

    // If truncated, change the final char in the buffer to a \0.
    // A negative result indicates a truncated buffer on Windows.
    if (result < 0 || result >= buf_len)
    {
        buf[buf_len - 1] = '\0';
        result = buf_len - 1;
    }

    return result;
}

// Safe, portable snprintf().
int TXT_snprintf(char *buf, size_t buf_len, const char *s, ...)
{
    va_list args;
    int result;
    va_start(args, s);
    result = TXT_vsnprintf(buf, buf_len, s, args);
    va_end(args);
    return result;
}

