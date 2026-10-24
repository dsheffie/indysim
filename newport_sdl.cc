#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include "newport_sdl.hh"
#include "sgi_newport.hh"
#include "interpret.hh"
#include "sgi_ps2.hh"

#ifdef HAVE_SDL
#include <SDL.h>

/* SDL keycode -> sgi_ps2 key name */
static const char *sdl_key_name(SDL_Keycode k) {
  switch(k)
    {
    case SDLK_a: return "a"; case SDLK_b: return "b"; case SDLK_c: return "c"; case SDLK_d: return "d";
    case SDLK_e: return "e"; case SDLK_f: return "f"; case SDLK_g: return "g"; case SDLK_h: return "h";
    case SDLK_i: return "i"; case SDLK_j: return "j"; case SDLK_k: return "k"; case SDLK_l: return "l";
    case SDLK_m: return "m"; case SDLK_n: return "n"; case SDLK_o: return "o"; case SDLK_p: return "p";
    case SDLK_q: return "q"; case SDLK_r: return "r"; case SDLK_s: return "s"; case SDLK_t: return "t";
    case SDLK_u: return "u"; case SDLK_v: return "v"; case SDLK_w: return "w"; case SDLK_x: return "x";
    case SDLK_y: return "y"; case SDLK_z: return "z";
    case SDLK_1: return "1"; case SDLK_2: return "2"; case SDLK_3: return "3"; case SDLK_4: return "4";
    case SDLK_5: return "5"; case SDLK_6: return "6"; case SDLK_7: return "7"; case SDLK_8: return "8";
    case SDLK_9: return "9"; case SDLK_0: return "0";
    case SDLK_MINUS: return "-"; case SDLK_EQUALS: return "="; case SDLK_LEFTBRACKET: return "[";
    case SDLK_RIGHTBRACKET: return "]"; case SDLK_SEMICOLON: return ";"; case SDLK_QUOTE: return "'";
    case SDLK_BACKQUOTE: return "`"; case SDLK_BACKSLASH: return "\\"; case SDLK_COMMA: return ",";
    case SDLK_PERIOD: return "."; case SDLK_SLASH: return "/";
    case SDLK_ESCAPE: return "esc"; case SDLK_BACKSPACE: return "backspace"; case SDLK_TAB: return "tab";
    case SDLK_RETURN: return "enter"; case SDLK_SPACE: return "space"; case SDLK_CAPSLOCK: return "capslock";
    case SDLK_LCTRL: case SDLK_RCTRL: return "ctrl";
    case SDLK_LSHIFT: return "shift"; case SDLK_RSHIFT: return "rshift";
    case SDLK_LALT: case SDLK_RALT: return "alt";
    case SDLK_F1: return "f1"; case SDLK_F2: return "f2"; case SDLK_F3: return "f3"; case SDLK_F4: return "f4";
    case SDLK_F5: return "f5"; case SDLK_F6: return "f6"; case SDLK_F7: return "f7"; case SDLK_F8: return "f8";
    case SDLK_F9: return "f9"; case SDLK_F10: return "f10"; case SDLK_F11: return "f11"; case SDLK_F12: return "f12";
    case SDLK_INSERT: return "insert"; case SDLK_DELETE: return "delete"; case SDLK_HOME: return "home";
    case SDLK_END: return "end"; case SDLK_PAGEUP: return "pgup"; case SDLK_PAGEDOWN: return "pgdn";
    case SDLK_UP: return "up"; case SDLK_DOWN: return "down"; case SDLK_LEFT: return "left"; case SDLK_RIGHT: return "right";
    default: return nullptr;
    }
}

/* SDL mouse state -> PS/2 buttons (bit0 left, bit1 right, bit2 middle) */
static uint8_t ps2_buttons() {
  const uint32_t m = SDL_GetMouseState(nullptr, nullptr);
  uint8_t b = 0;
  if(m & SDL_BUTTON_LMASK) {
    b |= 0x01;
  }
  if(m & SDL_BUTTON_RMASK) {
    b |= 0x02;
  }
  if(m & SDL_BUTTON_MMASK) {
    b |= 0x04;
  }
  return b;
}

newport_sdl::newport_sdl(sgi_newport *np, state_t *s) : np(np), s(s) {
  if(SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[newport] sdl: SDL_Init failed: %s\n", SDL_GetError());
    return;
  }
  const int w = (int)sgi_newport::DISP_W, h = (int)sgi_newport::DISP_H;
  /* the window rarely gets an integer scale of 1280x1024 (and on a HiDPI display it is
   * sized in points, not pixels), so nearest-neighbour drops whole rows/columns and
   * breaks up text; filter linearly instead */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  /* open at the largest 5:4 size that fits the display's usable area (a laptop cannot
   * show 1280x1024 points, and the OS then shrinks the window) */
  int ww = w, wh = h;
  SDL_Rect ub;
  if(SDL_GetDisplayUsableBounds(0, &ub) == 0 && (ub.w < w || ub.h < h)) {
    const double sc = std::min((double)ub.w / w, (double)(ub.h - 40) / h);   /* 40: title bar */
    ww = (int)(w * sc);
    wh = (int)(h * sc);
  }
  /* ALLOW_HIGHDPI: render at the display's real pixel density (Retina = 2x points) */
  SDL_Window *wn = SDL_CreateWindow("indysim Newport", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if(wn == nullptr) {
    fprintf(stderr, "[newport] sdl: CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return;
  }
  SDL_Renderer *rn = SDL_CreateRenderer(wn, -1, SDL_RENDERER_ACCELERATED);   /* no vsync: never block the sim */
  if(rn == nullptr) {
    rn = SDL_CreateRenderer(wn, -1, 0);
  }
  win = wn;
  ren = rn;
  tex = SDL_CreateTexture(rn, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  SDL_RenderSetLogicalSize(rn, w, h);
  pixels = new uint32_t[(size_t)w * h];
  abs_mouse = getenv("NEWPORT_SDL_ABS") != nullptr;
  if(abs_mouse) {
    SDL_ShowCursor(SDL_DISABLE);   /* only the guest's cursor is meaningful */
  }
  fprintf(stderr, "[newport] sdl: window up (%dx%d); close it to stop the run\n", w, h);
}

newport_sdl::~newport_sdl() {
  delete [] pixels;
  if(tex) {
    SDL_DestroyTexture((SDL_Texture *)tex);
  }
  if(ren) {
    SDL_DestroyRenderer((SDL_Renderer *)ren);
  }
  if(win) {
    SDL_DestroyWindow((SDL_Window *)win);
    SDL_Quit();
  }
}

void newport_sdl::set_grab(bool on) {
  if(on == grabbed) {
    return;
  }
  grabbed = on;
  if(!abs_mouse) {
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
  }
  SDL_SetWindowTitle((SDL_Window *)win, on ? "indysim Newport -- captured; Ctrl+Alt+G to release"
                                           : "indysim Newport -- click to capture keyboard+mouse");
}

void newport_sdl::abs_mouse_step() {
  /* wait until the last move has been read, so the VC2 position is not stale */
  if(!s->ps2->aux_idle()) {
    return;
  }
  int wx = 0, wy = 0, ww = 0, wh = 0;
  SDL_GetMouseState(&wx, &wy);
  SDL_GetWindowSize((SDL_Window *)win, &ww, &wh);
  if(ww <= 0 || wh <= 0) {
    return;
  }
  /* window -> framebuffer, undoing SDL_RenderSetLogicalSize's letterbox */
  const int fw = (int)sgi_newport::DISP_W, fh = (int)sgi_newport::DISP_H;
  const double sc = std::min((double)ww / fw, (double)wh / fh);
  const double ox = (ww - fw * sc) / 2.0, oy = (wh - fh * sc) / 2.0;
  const int tx = (int)((wx - ox) / sc), ty = (int)((wy - oy) / sc);
  /* steer only toward a NEW host position: once there, leave the guest cursor alone
   * so scripted input (NEWPORT_INPUT[_FIFO]) is not pulled back */
  if(tx != abs_last_tx || ty != abs_last_ty) {
    abs_last_tx = tx;
    abs_last_ty = ty;
    abs_settled = false;
  }
  if(abs_settled) {
    return;
  }
  int cx = 0, cy = 0;
  np->cursor_pos(cx, cy);
  int ex = tx - (cx - 31), ey = ty - (cy - 31);   /* image corner; the hotspot is a fixed offset */
  if(ex == 0 && ey == 0) {
    abs_settled = true;
    return;
  }
  /* small moves go 1:1; larger ones at half gain since the X server accelerates them */
  if(ex > 4 || ex < -4) {
    ex /= 2;
  }
  if(ey > 4 || ey < -4) {
    ey /= 2;
  }
  s->ps2->mouse(ex, ey, s->ps2->buttons());
}

void newport_sdl::pump() {
  if(win == nullptr) {
    return;
  }
  int mdx = 0, mdy = 0;
  bool mouse_changed = false;
  SDL_Event e;
  while(SDL_PollEvent(&e)) {
    switch(e.type)
      {
      case SDL_QUIT:
        s->brk = 1;
        return;
      case SDL_WINDOWEVENT:
        if(e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
          set_grab(false);
        }
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        if(e.key.repeat || !grabbed || s->ps2 == nullptr) {
          break;
        }
        const uint16_t mods = SDL_GetModState();
        if(e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_g && (mods & KMOD_CTRL) && (mods & KMOD_ALT)) {
          set_grab(false);
          s->ps2->key("ctrl", false);
          s->ps2->key("alt", false);
          break;
        }
        const char *nm = sdl_key_name(e.key.keysym.sym);
        if(nm) {
          s->ps2->key(nm, e.type == SDL_KEYDOWN);
        }
        break;
      }
      case SDL_MOUSEMOTION:
        if(grabbed && !abs_mouse) {
          mdx += e.motion.xrel;
          mdy += e.motion.yrel;
          mouse_changed = true;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(!grabbed) {                         /* the first click captures; consume it */
          set_grab(true);
          break;
        }
        mouse_changed = true;
        break;
      case SDL_MOUSEBUTTONUP:
        if(grabbed) {
          mouse_changed = true;
        }
        break;
      default:
        break;
      }
  }
  if(mouse_changed && s->ps2) {                /* one packet per pump: coalesced motion */
    s->ps2->mouse(mdx, mdy, ps2_buttons());
  }
  if(abs_mouse && grabbed && s->ps2) {
    abs_mouse_step();
  }
  const uint32_t now = SDL_GetTicks();
  /* NEWPORT_SDL_MS: minimum ms between frames (default 33 ~ 30 fps).  Each present
   * pushes a full 1280x1024 frame and runs in the sim loop, so over forwarded X11
   * use a few hundred ms. */
  static const uint32_t g_frame_ms = getenv("NEWPORT_SDL_MS") ? (uint32_t)strtoul(getenv("NEWPORT_SDL_MS"), nullptr, 0) : 33;
  if(now - last_render_ms < g_frame_ms) {
    return;
  }
  last_render_ms = now;
  /* nothing written to the board since the last frame: the picture is unchanged, so
   * skip the render + upload + present (an idle desktop then costs nothing, which
   * matters most over forwarded X11) */
  if(np->display_gen() == last_gen) {
    return;
  }
  last_gen = np->display_gen();
  np->render(pixels);
  SDL_UpdateTexture((SDL_Texture *)tex, nullptr, pixels, (int)(sgi_newport::DISP_W * 4));
  SDL_RenderClear((SDL_Renderer *)ren);
  SDL_RenderCopy((SDL_Renderer *)ren, (SDL_Texture *)tex, nullptr, nullptr);
  SDL_RenderPresent((SDL_Renderer *)ren);
}

#else

newport_sdl::newport_sdl(sgi_newport *np, state_t *s) : np(np), s(s) {
  fprintf(stderr, "[newport] sdl: indysim was built without SDL2 (use NEWPORT_PPM=<file> instead)\n");
}

newport_sdl::~newport_sdl() {
}

void newport_sdl::pump() {
}

#endif
