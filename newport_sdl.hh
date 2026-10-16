#ifndef __NEWPORT_SDL_HH__
#define __NEWPORT_SDL_HH__

#include <cstdint>

class sgi_newport;
struct state_t;

/* newport_sdl: a live SDL2 window showing the Newport display (interp_mips --sdl),
 * with keyboard and mouse fed to the IOC2 PS/2 model (sgi_ps2).  Clicking the
 * window captures input (relative mouse mode); Ctrl+Alt+G releases it.  SDL types stay out of this header (opaque void*) so
 * only newport_sdl.cc needs SDL; without HAVE_SDL the constructor reports that
 * and ok() is false.  pump() is called on each vertical retrace; it polls window
 * events (closing the window sets s->brk) and re-renders at most ~30 Hz of wall
 * clock. */
class newport_sdl {
public:
  newport_sdl(sgi_newport *np, state_t *s);
  ~newport_sdl();
  bool ok() const { return win != nullptr; }
  void pump();
private:
  sgi_newport *np;
  state_t *s;
  void *win = nullptr, *ren = nullptr, *tex = nullptr;
  uint32_t *pixels = nullptr;
  uint32_t last_render_ms = 0;
  bool grabbed = false;
  void set_grab(bool on);
};

#endif
