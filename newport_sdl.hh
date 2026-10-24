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
  uint64_t last_gen = ~0ULL;   /* sgi_newport::disp_gen at the last frame we pushed */
  bool grabbed = false;
  /* NEWPORT_SDL_ABS: drive the guest cursor toward the host pointer (closed loop on
   * the VC2 cursor position) instead of relative mode -- relative mode's pointer
   * warping fights a VNC/remote-X client's absolute positions */
  bool abs_mouse = false;
  int abs_last_tx = -1, abs_last_ty = -1;   /* host target at the last step */
  bool abs_settled = true;                  /* guest cursor reached the current target */
  void abs_mouse_step();
  void set_grab(bool on);
};

#endif
