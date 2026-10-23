/* worker.js -- runs the indysim WASM module off the main thread.
 *
 * Flow (INVOKE_RUN=0): the page posts a {cmd:'boot', mode, disk?, prom?} message.
 * We instantiate the module, for IRIX mount a user-picked disk image + real SGI
 * PROM via WORKERFS (read-only, lazy slice reads -- images are multi-GB), then
 * callMain. Serial
 * output arrives via emscripten stdout (print); Newport frames arrive from the
 * module's EM_JS web_post_frame (also runs here) -- both postMessage'd to the page.
 */
self.importScripts('interp_mips.js');   /* defines self.createIndysim (MODULARIZE) */

let started = false;
let Module = null;   /* hoisted so post-boot input messages can reach it */

self.onmessage = async (e) => {
  const msg = e.data || {};

  /* input events arrive in their own messages after boot; forward to the PS/2
   * model. Dropped silently until the module exists (module scope `Module`). */
  if(msg.cmd === 'key') {
    if(Module) { Module.ccall('web_key_event', 'void', ['number', 'string'], [msg.down ? 1 : 0, msg.name]); }
    return;
  }
  if(msg.cmd === 'mouse') {
    if(Module) { Module.ccall('web_mouse_event', 'void', ['number', 'number', 'number'], [msg.dx | 0, msg.dy | 0, msg.buttons | 0]); }
    return;
  }

  if(msg.cmd !== 'boot' || started) { return; }
  started = true;

  Module = await createIndysim({
    print:    (line) => self.postMessage({ kind: 'serial', text: line + '\n' }),
    printErr: (line) => self.postMessage({ kind: 'diag',   text: line + '\n' }),
  });

  let args;
  if(msg.mode === 'irix' && msg.disk && msg.prom) {
    /* bring-your-own IRIX: mount the user's disk image AND a real SGI PROM, both
     * read-only via WORKERFS (never baked into the build -- both are proprietary).
     * The real PROM does the whole boot itself (SCSI probe, volume header, sash,
     * XFS, jump to IRIX); we do NOT side-load a kernel. Disk writes land in
     * sgi_scsi's in-memory COW overlay, so the image stays untouched. */
    try {
      Module.FS.mkdir('/disk');
      Module.FS.mount(Module.FS.filesystems.WORKERFS, { files: [msg.disk] }, '/disk');
      Module.FS.mkdir('/prom');
      Module.FS.mount(Module.FS.filesystems.WORKERFS, { files: [msg.prom] }, '/prom');
    } catch(err) {
      self.postMessage({ kind: 'diag', text: '[worker] WORKERFS mount failed: ' + err + '\n' });
      return;
    }
    args = ['--prom', '/prom/' + msg.prom.name, '--start-pc', '0xbfc00000',
            '--disk', '/disk/' + msg.disk.name, '--newport'];
  } else {
    /* public Linux demo: no disk; --newport gives the Newport console + PS/2 input */
    args = ['-f', '/vmlinux.32', '--prom', '/henry_arcs.bin', '--start-pc', '0xbfc00000', '--newport'];
  }

  self.postMessage({ kind: 'diag', text: '[worker] boot: ' + args.join(' ') + '\n' });
  Module.callMain(args);   /* runs the main loop; does not return */
};
