//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2018 fgsfds
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
// DESCRIPTION:
//	DOOM graphics stuff for SDL on the Vita.
//


#include "SDL.h"
#include "SDL_opengl.h"
#include "shader.h"
#include <vita2d.h>

#include "config.h"
#include "d_loop.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_input.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_diskicon.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "textscreen.h"

#define VITA_SCR_W 960
#define VITA_SCR_H 544

// These are (1) the window (or the full screen) that our game is rendered to
// and (2) the renderer that scales the texture (see below) into this window.
// These are also used in libtextscreen because the SDL port can't handle
// reinit.

SDL_Window *sdl_window;
SDL_Renderer *sdl_renderer;

// Current vita2d shader, used for sharp bilinear filtering

static vita2d_shader *shader = NULL;

// Vita2d texture for native rendering

static vita2d_texture *vitatex_hwscreen = NULL;
static uint8_t *vitatex_datap = NULL;

// Window title

static const char *window_title = "";

// This is the 320x200x8 paletted buffer that we draw to

static SDL_Surface *screenbuffer = NULL;

static SDL_Rect blit_rect =
{
    0,
    0,
    SCREENWIDTH,
    SCREENHEIGHT
};

static SDL_Rect target_rect =
{
    0,
    0,
    VITA_SCR_W,
    VITA_SCR_H
};

static float target_sx = 1.f;
static float target_sy = 1.f;

static uint32_t pixel_format;

// palette

static SDL_Color palette[256];
static boolean palette_to_set;

// display has been set up?

static boolean initialized = false;

// disable mouse?

static boolean nomouse = false;
int usemouse = 1;

// Save screenshots in PNG format.

int png_screenshots = 0;

// SDL video driver name

char *video_driver = "";

// Window position:

char *window_position = "center";

// Scaling filter applied

char *scaling_filter = "nearest";

// SDL display number on which to run.

int video_display = 0;

// Screen width and height, from configuration file.

int window_width = SCREENWIDTH * 2;
int window_height = SCREENHEIGHT_4_3 * 2;

// Fullscreen mode, 0x0 for SDL_WINDOW_FULLSCREEN_DESKTOP.

int fullscreen_width = 0, fullscreen_height = 0;

// Maximum number of pixels to use for intermediate scale buffer.

static int max_scaling_buffer_pixels = 16000000;

// Run in full screen mode?  (int type for config code)

int fullscreen = true;

// Aspect ratio correction mode

int aspect_ratio_correct = true;
static int actualheight;

// Force integer scales for resolution-independent rendering

int integer_scaling = false;

// VGA Porch palette change emulation

int vga_porch_flash = false;

// Force software rendering, for systems which lack effective hardware
// acceleration

int force_software_renderer = false;

// Time to wait for the screen to settle on startup before starting the
// game (ms)

static int startup_delay = 1000;

// Grab the mouse? (int type for config code). nograbmouse_override allows
// this to be temporarily disabled via the command line.

static int grabmouse = true;
static boolean nograbmouse_override = false;

// If this is set to true. right analog movement is translated to mouse
// movement.

static int emulate_mouse = false;

// The screen buffer; this is modified to draw things to the screen

pixel_t *I_VideoBuffer = NULL;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible = true;

// If true, we display dots at the bottom of the screen to 
// indicate FPS.

static boolean display_fps_dots;

// If this is true, the screen is rendered but not blitted to the
// video buffer.

static boolean noblit;

// Callback function to invoke to determine whether to grab the 
// mouse pointer.

static grabmouse_callback_t grabmouse_callback = NULL;

// Does the window currently have focus?

static boolean window_focused = true;

// Window resize state.

static boolean need_resize = false;
static unsigned int last_resize_time;
#define RESIZE_DELAY 500

// Gamma correction level to use

int usegamma = 0;

// Joystick/gamepad hysteresis
unsigned int joywait = 0;

void I_SetGrabMouseCallback(grabmouse_callback_t func)
{
    grabmouse_callback = func;
}

// Set the variable controlling FPS dots.

void I_DisplayFPSDots(boolean dots_on)
{
    display_fps_dots = dots_on;
}

// This should be called right before SDL_Quit() before the program exits.
// Vita SDL2 can't handle being reinitialized, so I put a shitton of hacks
// everywhere to make it use the same renderer and window in both i_video.c
// and txt_sdl.c, so the only point where we need to destroy them is the
// very end.

void I_VitaCleanupGraphics(void)
{
    vita2d_wait_rendering_done();
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    vita2d_free_texture(vitatex_hwscreen);
    vitatex_hwscreen = NULL;
    vitatex_datap = NULL;
    sdl_window = NULL;
    sdl_renderer = NULL;
}

// This attempts to use libtextscreen to draw a simple error popup box.

static int vita_show_error = 1;

static void ExitErrorMessage(TXT_UNCAST_ARG(widget), void *unused)
{
    vita_show_error = 0;
}

void I_VitaShowError(char *msg)
{
    txt_window_t *window;
    txt_window_action_t *close;

    vita2d_wait_rendering_done();

    if (!TXT_Init()) return;

    TXT_SetDesktopTitle(PACKAGE_STRING);
    window = TXT_NewWindow("Error");
    TXT_AddWidget(window, TXT_NewLabel(msg));
    close = TXT_NewWindowAction(KEY_ESCAPE, "Exit");
    TXT_SignalConnect(close, "pressed", ExitErrorMessage, NULL);

    TXT_SetWindowAction(window, TXT_HORIZ_RIGHT, close);
    TXT_SetWindowAction(window, TXT_HORIZ_LEFT, NULL);
    TXT_SetWindowPosition(window, TXT_HORIZ_CENTER, TXT_VERT_BOTTOM,
                                  TXT_SCREEN_W / 2, TXT_SCREEN_H - 9);

    vita_show_error = 1;
    while (vita_show_error)
    {
        TXT_DispatchEvents();
        TXT_DrawDesktop();
        TXT_Sleep(100);
    }

    TXT_Shutdown();
}

void I_ShutdownGraphics(void)
{
    if (initialized)
    {
        vita2d_wait_rendering_done();
        // don't deinit SDL here, makes shit crash
        initialized = false;
    }
}



//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?

}

static void HandleWindowEvent(SDL_WindowEvent *event)
{
    int i;

    switch (event->event)
    {
#if 0 // SDL2-TODO
        case SDL_ACTIVEEVENT:
            // need to update our focus state
            UpdateFocus();
            break;
#endif
        case SDL_WINDOWEVENT_EXPOSED:
            palette_to_set = true;
            break;

        case SDL_WINDOWEVENT_RESIZED:
            need_resize = true;
            last_resize_time = SDL_GetTicks();
            break;

        // Don't render the screen when the window is minimized:

        case SDL_WINDOWEVENT_MINIMIZED:
            screenvisible = false;
            break;

        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            screenvisible = true;
            break;

        // Update the value of window_focused when we get a focus event
        //
        // We try to make ourselves be well-behaved: the grab on the mouse
        // is removed if we lose focus (such as a popup window appearing),
        // and we dont move the mouse around if we aren't focused either.

        case SDL_WINDOWEVENT_FOCUS_GAINED:
            window_focused = true;
            break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
            window_focused = false;
            break;

        // We want to save the user's preferred monitor to use for running the
        // game, so that next time we're run we start on the same display. So
        // every time the window is moved, find which display we're now on and
        // update the video_display config variable.

        case SDL_WINDOWEVENT_MOVED:
            i = SDL_GetWindowDisplayIndex(sdl_window);
            if (i >= 0)
            {
                video_display = i;
            }
            break;

        default:
            break;
    }
}

extern void I_HandleKeyboardEvent(SDL_Event *sdlevent);
extern void I_HandleMouseEvent(SDL_Event *sdlevent);

// Mouse X and Y for mouse emulation.

static int mouse_dx = 0;
static int mouse_dy = 0;

// Mouse offers more precise turning control, so we can translate the right
// analog to mouse movements to achieve better control.

// We're using this private SDL function to send emulated mouse movements
// because I'm too lazy to do it "properly".

extern int SDL_SendMouseMotion(SDL_Window *window, Uint32 mouseID, int relative, int x, int y);

static void TranslateAnalogEvent(SDL_Event *ev)
{
    static const int deadzone = 512;
    static const int divider = 512;
    int delta;

    if (ev->jaxis.axis < 2)
        return;

    delta = ev->jaxis.value;
    if (delta >= -deadzone && delta <= deadzone)
        delta = 0;
    else
        delta /= divider;

    if (ev->jaxis.axis == 2)
        mouse_dx = delta;
    else if (ev->jaxis.axis == 3)
        mouse_dy = delta;
}

// No built-in touch support, so we translate mouse events to CTRL keypresses
// as actual mouse control is not really needed, since we got analogs and dpad

static void TranslateTouchEvent(SDL_Event *ev)
{
    SDL_Event ev_new;

    memset(&ev_new, 0, sizeof(SDL_Event));

    if (ev->tfinger.touchId == 0)
    {
        // front touch
        ev_new.key.keysym.sym = SDLK_TAB;
        ev_new.key.keysym.scancode = SDL_SCANCODE_TAB;
    }
    else
    {
        // back touch
        ev_new.key.keysym.sym = SDLK_PAGEUP;
        ev_new.key.keysym.scancode = SDL_SCANCODE_PAGEUP;
    }

    if (ev->type == SDL_FINGERDOWN)
    {
        ev_new.type = ev_new.key.type = SDL_KEYDOWN;
        ev_new.key.state = SDL_PRESSED;
    }
    else if (ev->type == SDL_FINGERUP)
    {
        ev_new.type = ev_new.key.type = SDL_KEYUP;
        ev_new.key.state = SDL_RELEASED;
    }

    I_HandleKeyboardEvent(&ev_new);
}

// A lot of actions can't be bound to joystick buttons in Chocolate, and in
// Heretic or Hexen joystick doesn't control the menus, so we have to translate
// joystick events to keys instead.

// HACK: I'm too fucking stupid to write a proper makefile that will recompile
//       this with -DHERETIC -DHEXEN or something, so we do this shit instead
extern boolean MenuActive __attribute__((weak));
extern boolean askforquit __attribute__((weak));
extern int messageToPrint __attribute__((weak));

static void TranslateControllerEvent(SDL_Event *ev)
{
    int btn;
    SDL_Event ev_new;
    boolean in_prompt;
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
    in_prompt = (&askforquit && askforquit) ||
        (&messageToPrint && messageToPrint);

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

    I_HandleKeyboardEvent(&ev_new);
}

void I_GetEvent(void)
{
    SDL_Event sdlevent;

    SDL_PumpEvents();

    while (SDL_PollEvent(&sdlevent))
    {
        switch (sdlevent.type)
        {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                I_HandleKeyboardEvent(&sdlevent);
                break;

            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
                if (usemouse && !nomouse)
                {
                    I_HandleMouseEvent(&sdlevent);
                }
                break;

            // Translate Vita front screen touches into keypresses
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
                TranslateTouchEvent(&sdlevent);
                break;

            // Translate some gamepad events to keys to allow menu navigation
            // in games that don't support joystick menu controls (Heretic, Hexen)
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                TranslateControllerEvent(&sdlevent);
                break;

            // Translate analog to mouse, if emulate_mouse is enabled
            case SDL_JOYAXISMOTION:
                if (emulate_mouse)
                {
                    TranslateAnalogEvent(&sdlevent);
                }
                break;

            case SDL_QUIT:
                if (screensaver_mode)
                {
                    I_Quit();
                }
                else
                {
                    event_t event;
                    event.type = ev_quit;
                    D_PostEvent(&event);
                }
                break;

            case SDL_WINDOWEVENT:
                if (sdlevent.window.windowID == SDL_GetWindowID(sdl_window))
                {
                    HandleWindowEvent(&sdlevent.window);
                }
                break;

            default:
                break;
        }
    }
}

//
// I_StartTic
//
void I_StartTic (void)
{
    if (!initialized)
    {
        return;
    }

    I_GetEvent();

    if (usemouse && !nomouse)
    {
        I_ReadMouse();
        if (emulate_mouse && (mouse_dx || mouse_dy))
            SDL_SendMouseMotion(sdl_window, 0, true, mouse_dx, mouse_dy);
    }

    if (joywait < I_GetTime())
    {
        I_UpdateJoystick();
    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}

// TODO: accelerate this
static inline void BlitBuffer(void)
{
    SDL_Color c;
    uint8_t *dst;
    const uint8_t *src, *end;

    src = screenbuffer->pixels;
    dst = vitatex_datap;
    end = src + SCREENWIDTH * SCREENHEIGHT;
    for (; src < end; ++src)
    {
        c = palette[*src];
        // ABGR
        *(dst++) = c.r;
        *(dst++) = c.g;
        *(dst++) = c.b;
        *(dst++) = 255;
    }
}

//01y
// [AM] Fractional part of the current tic, in the half-open
//      range of [0.0, 1.0).  Used for interpolation.
fixed_t fractionaltic;

//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
    static int lasttic;
    int tics;
    int i;

    if (!initialized)
        return;

    if (noblit)
        return;

    // draws little dots on the bottom of the screen

    if (display_fps_dots)
    {
        i = I_GetTime();
        tics = i - lasttic;
        lasttic = i;
        if (tics > 20) tics = 20;

        for (i=0 ; i<tics*4 ; i+=4)
            I_VideoBuffer[ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
        for ( ; i<20*4 ; i+=4)
            I_VideoBuffer[ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
    }

    // Draw disk icon before blit, if necessary.
    V_DrawDiskIcon();

    if (palette_to_set)
    {
        SDL_SetPaletteColors(screenbuffer->format->palette, palette, 0, 256);
        palette_to_set = false;

        if (vga_porch_flash)
        {
            // "flash" the pillars/letterboxes with palette changes, emulating
            // VGA "porch" behaviour (GitHub issue #832)
            SDL_SetRenderDrawColor(sdl_renderer, palette[0].r, palette[0].g,
                palette[0].b, SDL_ALPHA_OPAQUE);
            vita2d_set_clear_color(RGBA8(palette[0].r, palette[0].g,
                palette[0].b, 255));
        }
    }

    // Blit from the paletted 8-bit screen buffer to the intermediate
    // 32-bit RGBA buffer that we can load into the texture.

    // Have to blit manually here because it seems LowerBlit is bugged on Vita.
    // Blitting is done directly to native texture
    BlitBuffer();

    // Native rendering, bypassing any SDL2 functions that might slow things down
    vita2d_start_drawing();
    if (vga_porch_flash) vita2d_clear_screen();
    vita2d_draw_texture_scale(
        vitatex_hwscreen,
        target_rect.x, target_rect.y,
        target_sx, target_sy
    );
    vita2d_end_drawing();
    vita2d_swap_buffers();

    //01y
    if (crispy->uncapped)
    {
	fractionaltic = I_GetTimeMS() * TICRATE % 1000 * FRACUNIT / 1000;
    }

    // Restore background and undo the disk indicator, if it was drawn.
    V_RestoreDiskBackground();
}


//
// I_ReadScreen
//
void I_ReadScreen (pixel_t* scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH*SCREENHEIGHT*sizeof(*scr));
}


//
// I_SetPalette
//
void I_SetPalette (byte *doompalette)
{
    int i;

    for (i=0; i<256; ++i)
    {
        // Zero out the bottom two bits of each channel - the PC VGA
        // controller only supports 6 bits of accuracy.

        palette[i].r = gammatable[usegamma][*doompalette++] & ~3;
        palette[i].g = gammatable[usegamma][*doompalette++] & ~3;
        palette[i].b = gammatable[usegamma][*doompalette++] & ~3;
    }

    palette_to_set = true;
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex(int r, int g, int b)
{
    int best, best_diff, diff;
    int i;

    best = 0; best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
        diff = (r - palette[i].r) * (r - palette[i].r)
             + (g - palette[i].g) * (g - palette[i].g)
             + (b - palette[i].b) * (b - palette[i].b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

// 
// Set the window title
//

void I_SetWindowTitle(const char *title)
{
    window_title = title;
}

//
// Call the SDL function to set the window title, based on 
// the title set with I_SetWindowTitle.
//

void I_InitWindowTitle(void)
{

}

// Set the application icon

void I_InitWindowIcon(void)
{

}

static void CalculateTargetRect(void)
{
    double aspect;
    int scale;

    if (aspect_ratio_correct == 2)
    {
        // special case: fit to screen
        target_rect.x = 0;
        target_rect.y = 0;
        target_rect.w = VITA_SCR_W;
        target_rect.h = VITA_SCR_H;
        return;
    }

    if (integer_scaling)
    {
        scale = VITA_SCR_H / actualheight;
        target_rect.w = SCREENWIDTH * scale;
        target_rect.h = actualheight * scale;
        target_rect.y = VITA_SCR_H / 2 - target_rect.h / 2;
    }
    else
    {
        aspect = (double)SCREENWIDTH / (double)actualheight;
        target_rect.h = VITA_SCR_H;
        target_rect.w = VITA_SCR_H * aspect;
        target_rect.y = 0;
    }

    target_rect.x = VITA_SCR_W / 2 - target_rect.w / 2;
}

void I_GetWindowPosition(int *x, int *y, int w, int h)
{
    *x = *y = 0;
}

void I_GraphicsCheckCommandLine(void)
{
    //!
    // @category video
    // @vanilla
    //
    // Disable blitting the screen.
    //

    noblit = M_CheckParm ("-noblit");

    //!
    // @category video 
    //
    // Don't grab the mouse when running in windowed mode.
    //

    nograbmouse_override = M_ParmExists("-nograbmouse");

    //!
    // @category video 
    //
    // Disable the mouse.
    //

    nomouse = M_CheckParm("-nomouse") > 0;
}

// Check if we have been invoked as a screensaver by xscreensaver.

void I_CheckIsScreensaver(void)
{
    screensaver_mode = false;
}

// This is also used in txt_sdl.c.

void I_VitaInitSDLWindow(void)
{
    int x, y, w, h, renderer_flags;
    SDL_DisplayMode mode;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
    {
        if (SDL_Init(SDL_INIT_VIDEO) < 0)
        {
            I_Error("Failed to initialize video: %s", SDL_GetError());
        }
    }

    if (sdl_window == NULL)
    {
        w = VITA_SCR_W;
        h = VITA_SCR_H;
        x = SDL_WINDOWPOS_UNDEFINED;
        y = SDL_WINDOWPOS_UNDEFINED;

        sdl_window = SDL_CreateWindow(NULL, x, y, w, h, SDL_WINDOW_SHOWN);

        if (sdl_window == NULL)
        {
            I_Error("Error creating window for video startup: %s",
                SDL_GetError());
        }
    }

    if (SDL_GetCurrentDisplayMode(video_display, &mode) != 0)
    {
        I_Error("Could not get display mode for video display #%d: %s",
            video_display, SDL_GetError());
    }

    if (sdl_renderer == NULL)
    {
        renderer_flags = 0;
        // Turn on vsync if we aren't in a -timedemo
        if (!singletics && mode.refresh_rate > 0)
        {
            renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
        }

        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, renderer_flags);

        if (sdl_renderer == NULL)
        {
            I_Error("Error creating renderer for screen window: %s",
                    SDL_GetError());
        }

        vita2d_texture_set_alloc_memblock_type(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
        vita2d_set_vblank_wait(!!(renderer_flags & SDL_RENDERER_PRESENTVSYNC));
        vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    }
}

static void SetVideoMode(void)
{
    SceGxmTextureFilter filter;

    window_width = VITA_SCR_W;
    window_height = VITA_SCR_H;
    pixel_format = SDL_PIXELFORMAT_ABGR8888;

    if (!sdl_renderer || !sdl_window)
    {
        I_VitaInitSDLWindow();
    }

    // Set up to render directly into a vita-native texture

    if (!vitatex_hwscreen)
    {
        vitatex_hwscreen = vita2d_create_empty_texture_format(
            SCREENWIDTH, SCREENHEIGHT,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8
        );
        vitatex_datap = vita2d_texture_get_datap(vitatex_hwscreen);
    }

    // Set the scaling quality for rendering and immediate texture.
    // Defaults to "nearest", which is gritty and pixelated and resembles
    // software scaling pretty well.  "linear" can be set as an alternative,
    // which may give better results at low resolutions.

    if (!strcmp(scaling_filter, "linear"))
    {
        filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
    }
    else if (!strcmp(scaling_filter, "sharp"))
    {
        // For the sharp_bilinear_simple shader to work, linear filtering has to be enabled.
        // This is done by a simple command here, supported by SDL2 for Vita since 2017/12/24.
        // This affects all textures created after this command.
        // Enable sharp-bilinear-simple shader for sharp pixels without distortion.
        // This has to be done after the SDL renderer is created because that inits vita2d.
        shader = Vita_SetShader(VSH_SHARP_BILINEAR_SIMPLE);
        filter = SCE_GXM_TEXTURE_FILTER_LINEAR;
    }
    else if (!strcmp(scaling_filter, "scale2x"))
    {
        shader = Vita_SetShader(VSH_SCALE2X);
        filter = SCE_GXM_TEXTURE_FILTER_POINT;
    }
    else
    {
        scaling_filter = "nearest";
        filter = SCE_GXM_TEXTURE_FILTER_POINT;
    }

    vita2d_texture_set_filters(vitatex_hwscreen, filter, filter);

    // erase the screen for both buffers
    for (int i = 0; i <= 3; i++)
    {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    // Create the 8-bit paletted screenbuffer surface.

    if (screenbuffer == NULL)
    {
        screenbuffer = SDL_CreateRGBSurface(0,
                                            SCREENWIDTH, SCREENHEIGHT, 8,
                                            0, 0, 0, 0);
        SDL_FillRect(screenbuffer, NULL, 0);
    }

    CalculateTargetRect();

    target_sx = (float) target_rect.w / (float) blit_rect.w;
    target_sy = (float) target_rect.h / (float) blit_rect.h;
}

void I_InitGraphics(void)
{
    SDL_Event dummy;
    byte *doompal;

    fullscreen = true;

    if (aspect_ratio_correct)
    {
        actualheight = SCREENHEIGHT_4_3;
    }
    else
    {
        actualheight = SCREENHEIGHT;
    }

    // Create the game window; this may switch graphic modes depending
    // on configuration.
    SetVideoMode();

    // Start with a clear black screen
    // (screen will be flipped after we set the palette)

    SDL_FillRect(screenbuffer, NULL, 0);

    // Set the palette

    doompal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_CACHE);
    I_SetPalette(doompal);
    SDL_SetPaletteColors(screenbuffer->format->palette, palette, 0, 256);

    // On some systems, it takes a second or so for the screen to settle
    // after changing modes.  We include the option to add a delay when
    // setting the screen mode, so that the game doesn't start immediately
    // with the player unable to see anything.

    if (fullscreen && !screensaver_mode)
    {
        SDL_Delay(startup_delay);
    }

    // The actual 320x200 canvas that we draw to. This is the pixel buffer of
    // the 8-bit paletted screen buffer that gets blit on an intermediate
    // 32-bit RGBA screen buffer that gets loaded into a texture that gets
    // finally rendered into our window or full screen in I_FinishUpdate().

    I_VideoBuffer = screenbuffer->pixels;
    V_RestoreBuffer();

    // Clear the screen to black.

    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);

    // clear out any events waiting at the start and center the mouse

    if (emulate_mouse)
    {
        SDL_SetRelativeMouseMode(true);
        SDL_GetRelativeMouseState(NULL, NULL);
    }

    while (SDL_PollEvent(&dummy));

    initialized = true;

    // Call I_ShutdownGraphics on quit

    I_AtExit(I_ShutdownGraphics, true);
}

// Bind all variables controlling video options into the configuration
// file system.
void I_BindVideoVariables(void)
{
    M_BindIntVariable("use_mouse",                 &usemouse);
    M_BindIntVariable("fullscreen",                &fullscreen);
    M_BindIntVariable("video_display",             &video_display);
    M_BindIntVariable("aspect_ratio_correct",      &aspect_ratio_correct);
    M_BindIntVariable("integer_scaling",           &integer_scaling);
    M_BindIntVariable("vga_porch_flash",           &vga_porch_flash);
    M_BindIntVariable("startup_delay",             &startup_delay);
    M_BindIntVariable("fullscreen_width",          &fullscreen_width);
    M_BindIntVariable("fullscreen_height",         &fullscreen_height);
    M_BindIntVariable("force_software_renderer",   &force_software_renderer);
    M_BindIntVariable("max_scaling_buffer_pixels", &max_scaling_buffer_pixels);
    M_BindIntVariable("window_width",              &window_width);
    M_BindIntVariable("window_height",             &window_height);
    M_BindIntVariable("grabmouse",                 &grabmouse);
    M_BindStringVariable("video_driver",           &video_driver);
    M_BindStringVariable("window_position",        &window_position);
    M_BindIntVariable("usegamma",                  &usegamma);
    M_BindIntVariable("png_screenshots",           &png_screenshots);
    M_BindStringVariable("scaling_filter",         &scaling_filter);
    M_BindIntVariable("emulate_mouse",             &emulate_mouse);
}
