// Steers the player's craft for the next racing buoy while tools/shader_plan.json holds the gas, so a recording
// sees the whole course rather than the open sea beside it. Run by the plan with evalFile; window.autopilot(false)
// stops it. It reads, every 30 ms: the camera in use (gCameraPerspective, 0x80227C80 + 0x10C an index, the index at
// 0x80223930), its position (+0x4C, +0x54) and horizontal heading (+0x64, +0x6C, docs/GAME-INTERNALS.md); the racing
// buoys (0x801AEE20, 0x104 each, x at +0 and z at +8, their count at 0x801BB120, patches/buoys.cpp); and the buoys
// the player has passed (standing +0xC, 0x801C2938 + 0x378 a slot, the slot at 0x800D48DC). Then it holds the arrow
// key that turns toward the next buoy, as KeyboardEvents on the page (Emscripten's SDL reads those). A craft that has
// moved less than 40 units in 1.5 s is against a wall or a shore: it turns hard one way for 1.5 s, the other way the
// next time.
(() => {
  const f32 = (a) => new Float32Array(new Uint32Array([n64r(a)]).buffer)[0];
  const press = (key, down) => {
    const keyCode = key === 'ArrowLeft' ? 37 : 39;
    window.dispatchEvent(new KeyboardEvent(down ? 'keydown' : 'keyup', { key, code: key, keyCode, which: keyCode, bubbles: true }));
  };
  let held = null, stuckFrom = null, turnUntil = 0, turnKey = 'ArrowLeft';
  const hold = (key) => {
    if (held === key) return;
    if (held) press(held, false);
    held = key;
    if (held) press(held, true);
  };
  clearInterval(window.autopilotTimer);
  window.autopilot = (on) => { if (!on) { clearInterval(window.autopilotTimer); hold(null); } };
  window.autopilotTimer = setInterval(() => {
    if (n64r(0x800DAB24) !== 0x28) { hold(null); return; }
    const cam = 0x80227C80 + n64r(0x80223930) * 0x10C;
    const cx = f32(cam + 0x4C), cz = f32(cam + 0x54), hx = f32(cam + 0x64), hz = f32(cam + 0x6C);
    const count = n64r(0x801BB120);
    if (!count || count > 64) { hold(null); return; }
    const passed = n64r(0x801C2938 + n64r(0x800D48DC) * 0x378 + 0xC);
    const b = 0x801AEE20 + (passed % count) * 0x104;
    // Beside the buoy, on the side it is passed: +0x70 and +0x78 are the course's direction there, +0xC8 the side.
    const side = (n64r(b + 0xC8) ? 1 : -1) * (window.autopilotSide ?? 1) * 150;
    const dx = f32(b) - f32(b + 0x78) * side - cx, dz = f32(b + 8) + f32(b + 0x70) * side - cz;
    const standing = 0x801C2938 + n64r(0x800D48DC) * 0x378;
    if (passed !== window.autopilotPassed) {
      window.autopilotPassed = passed;
      (window.autopilotSides ??= []).push(n64r(standing + 0x28));
    }
    const now = performance.now();
    if (now < turnUntil) { hold(turnKey); return; }
    if (!stuckFrom || Math.hypot(cx - stuckFrom.x, cz - stuckFrom.z) > 40) stuckFrom = { x: cx, z: cz, t: now };
    else if (now - stuckFrom.t > 1500) {
      turnKey = turnKey === 'ArrowLeft' ? 'ArrowRight' : 'ArrowLeft';
      turnUntil = now + 1500;
      stuckFrom = null;
      window.autopilotStuck = (window.autopilotStuck ?? 0) + 1;
      return;
    }
    const cross = hx * dz - hz * dx, dot = hx * dx + hz * dz;
    const angle = Math.atan2(cross, dot);
    window.autopilotLast = { passed, count, angle };
    hold(Math.abs(angle) < 0.08 ? null : (angle > 0) === (window.autopilotSign ?? true) ? 'ArrowRight' : 'ArrowLeft');
  }, 30);
})();
