/* diskOS v6 / v1 - the scroll show.
   One sticky theatre holds three layers (the pressed disc, the eight worlds, the lit stage).
   Scroll position drives which layer is on and how it hands over to the next; the only things
   that move on their own are the light on the disc and the orbit, and both stop with the pause
   control, off screen, in a hidden tab, or on a device that cannot keep up. */
(function () {
  'use strict';
  var root = document.documentElement;
  if (!root.classList.contains('js')) root.classList.add('js');
  var reduceMQ = window.matchMedia('(prefers-reduced-motion: reduce)');
  var fineMQ = window.matchMedia('(pointer: fine)');
  var savedPaused = false;
  try { savedPaused = localStorage.getItem('diskos-motion-paused') === '1'; } catch (e) { /* use the session default */ }
  root.classList.toggle('full', !reduceMQ.matches);
  root.classList.toggle('calm', reduceMQ.matches);
  root.classList.toggle('intro', !reduceMQ.matches && !savedPaused);
  root.classList.toggle('paused', savedPaused);

  function start() {

  var THEMES = [
    { key: 'default', name: 'halo', song: 'Between Tides', artist: 'Iona Relay', rgb: '255,55,95' },
    { key: 'something', name: 'something', song: 'Open Circuit', artist: 'Decimal Choir', rgb: '255,74,58' },
    { key: 'hi-fi', name: 'hi-fi', song: 'Amber Window', artist: 'Caldera Lines', rgb: '255,166,48' },
    { key: 'terminal', name: 'terminal', song: 'Return Signal', artist: 'Port Meridian', rgb: '61,255,122' },
    { key: 'bauhaus', name: 'bauhaus', song: 'Red Interval', artist: 'Ada Assembly', rgb: '224,64,44' },
    { key: 'blueprint', name: 'blueprint', song: 'Section Three', artist: 'Datum Season', rgb: '110,170,255' },
    { key: 'stone', name: 'stone', song: 'Slow Strata', artist: 'Cairn Loom', rgb: '127,224,176' },
    { key: 'zine', name: 'zine', song: 'Leave A Mark', artist: 'Cut Paper Union', rgb: '255,61,165' }
  ];
  var ROSE = '255,55,95';

  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }
  function c01(v) { return clamp(v, 0, 1); }
  function lerp(a, b, t) { return a + (b - a) * t; }
  function sstep(a, b, v) { var t = c01((v - a) / (b - a)); return t * t * (3 - 2 * t); }
  function inOut(t) { return t < .5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2; }
  function outCubic(t) { return 1 - Math.pow(1 - t, 3); }
  function $(s, r) { return (r || document).querySelector(s); }
  function $$(s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); }

  /* Resolve the initial fragment only after the final layout and fonts are ready. */
  function restoreInitialHash() {
    var hash = location.hash, id;
    if (!hash || hash === '#main') return;
    try { id = decodeURIComponent(hash.slice(1)); } catch (e) { return; }
    var names = THEMES.map(function (t) { return t.name; });
    if (['top', 'tracklist', 'themes', 'install'].indexOf(id) < 0 && names.indexOf(id) < 0) return;
    var target = document.getElementById(id);
    if (!target) return;
    var cancelled = false;
    var inputs = ['wheel', 'touchstart', 'pointerdown', 'keydown', 'click', 'hashchange'];
    function cleanup() {
      inputs.forEach(function (type) { window.removeEventListener(type, cancel, true); });
      window.removeEventListener('load', ready);
    }
    function cancel() { cancelled = true; cleanup(); }
    function land() {
      cleanup();
      if (cancelled || location.hash !== hash) return;
      if (M) { measure(); kick(); }
      if (root.classList.contains('full')) {
        if (jump('#' + id)) return;
        var k = names.indexOf(id);
        if (k >= 0) {
          window.scrollTo({ top: Math.round(themeTop(k)), behavior: 'smooth' });
          return;
        }
      }
      target.scrollIntoView({ block: 'start', behavior: 'auto' });
    }
    function ready() {
      requestAnimationFrame(function () {
        if (cancelled) return;
        if (document.fonts && document.fonts.ready) {
          document.fonts.ready.then(function () { requestAnimationFrame(land); });
        } else requestAnimationFrame(land);
      });
    }
    inputs.forEach(function (type) { window.addEventListener(type, cancel, { capture: true, passive: true }); });
    if (document.readyState === 'complete') ready();
    else window.addEventListener('load', ready, { once: true });
  }

  /* ------------------------------------------------------------------
     The pressed-disc shader. A disc splits light by its track pitch; for each
     point we work out which wavelengths reach the eye from a light above the
     disc and colour the point with them (orders 1 and 2). The disc is drawn
     into a viewport-sized canvas from a centre and radius in pixels, so it can
     grow, shrink and travel without rescaling any bitmap.
     ------------------------------------------------------------------ */
  var VERT = 'attribute vec2 a;void main(){gl_Position=vec4(a,0.,1.);}';
  var FRAG = [
    'precision highp float;',
    'uniform vec2 uC;uniform float uR,uAA;',
    'uniform vec3 L;uniform float hub,b0,b1,track,glow,wid,fade;uniform vec3 tint;',
    'vec3 spec(float l){',
    '  vec3 c=vec3(0.);',
    '  if(l<380.||l>750.)return c;',
    '  if(l<440.)c=vec3(-(l-440.)/60.,0.,1.);',
    '  else if(l<490.)c=vec3(0.,(l-440.)/50.,1.);',
    '  else if(l<510.)c=vec3(0.,1.,-(l-510.)/20.);',
    '  else if(l<580.)c=vec3((l-510.)/70.,1.,0.);',
    '  else if(l<645.)c=vec3(1.,-(l-645.)/65.,0.);',
    '  else c=vec3(1.,0.,0.);',
    '  float f=l<420.?.3+.7*(l-380.)/40.:(l>700.?.3+.7*(750.-l)/50.:1.);',
    '  return c*f;',
    '}',
    'float h(vec2 p){return fract(sin(dot(p,vec2(12.9898,78.233)))*43758.5453);}',
    'void main(){',
    '  vec2 p=(gl_FragCoord.xy-uC)/uR;',
    '  float r=length(p);',
    '  if(r>1.||r<hub){gl_FragColor=vec4(0.);return;}',
    '  float edge=1.-smoothstep(1.-uAA,1.,r);',
    '  vec2 u=p/max(r,1e-4);vec2 t=vec2(-u.y,u.x);',
    '  vec3 P=vec3(p,0.);',
    '  vec3 V=vec3(0.,-.35,3.2);',
    '  vec3 i=normalize(P-L);vec3 o=normalize(V-P);',
    '  vec3 hv=normalize(-i+o);float sp=pow(max(hv.z,0.),420.);',
    '  vec3 col;',
    '  if(r<b0){',
    '    col=vec3(.012,.012,.016)+tint*.05;',
    '  }else if(r<b1){',
    '    float k=.5+.5*dot(normalize(L.xy-p),u);',
    '    col=vec3(.045,.046,.052)+vec3(.12,.123,.135)*pow(k,3.)+vec3(1.)*sp*.25;',
    '    col*=1.-.08*smoothstep(150.,400.,uR)*(.5+.5*sin(r*900.));',
    '  }else{',
    '    float fr=(r-b1)/(.985-b1)*8.;',
    '    float band=floor(fr);',
    '    float dg=min(fract(fr),1.-fract(fr))*(.985-b1)/8.*uR;',
    '    float gap=1.-.55*(1.-smoothstep(.35,1.4,dg));',
    '    float on=clamp(1.-abs(band-track),0.,1.);',
    '    float phi=atan(p.y,p.x);float th=atan(L.y,L.x);',
    '    float d1=mod(phi-th+3.14159,6.28318)-3.14159;',
    '    float side=abs(d1)>1.5708?-1.:1.;',
    '    float dd=d1;if(dd>1.5708)dd-=3.14159;if(dd<-1.5708)dd+=3.14159;',
    '    float rr=(r-b1)/(.985-b1);',
    '    float shear=(rr-.5)*.42*side;',
    '    float x1=(dd+shear)/wid;',
    '    vec3 d=spec(565.+x1*175.*side)*smoothstep(1.25,.75,abs(x1));',
    '    float x2=(dd+shear*1.7-.5*side*wid)/(wid*1.9);',
    '    d+=spec(565.+x2*175.*side)*smoothstep(1.2,.7,abs(x2))*.32;',
    '    float fd=smoothstep(0.,.22,rr)*(.55+.45*rr);',
    '    float streak=exp(-pow(dd/.018,2.))*(side>0.?1.:.35)*(.25+.75*rr);',
    '    col=vec3(.028,.029,.034)+d*fd*(.82+.9*on)+vec3(.9,.92,1.)*streak*.45+vec3(1.)*sp*.5;',
    '    col+=vec3(.13,.035,.06)*on*(.6+.4*sin(rr*240.));',
    '    col*=gap*(.96+.04*h(floor(gl_FragCoord.xy)));',
    '    if(r>.985)col=vec3(.16,.165,.18)+vec3(.5)*pow(max(dot(normalize(L.xy-p),u),0.),3.);',
    '  }',
    '  col+=tint*glow*exp(-(r-hub)*7.);',
    '  float a=edge*fade;',
    '  gl_FragColor=vec4(col*a,a);',
    '}'
  ].join('\n');

  function makeDisc(canvas, keep) {
    var gl = null;
    try { gl = canvas.getContext('webgl', { premultipliedAlpha: true, antialias: false, alpha: true, depth: false, stencil: false, preserveDrawingBuffer: !!keep }); } catch (e) { gl = null; }
    if (!gl) return null;
    function sh(type, src) {
      var s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error('shader');
      return s;
    }
    var pr = gl.createProgram();
    gl.attachShader(pr, sh(gl.VERTEX_SHADER, VERT)); gl.attachShader(pr, sh(gl.FRAGMENT_SHADER, FRAG));
    gl.linkProgram(pr);
    if (!gl.getProgramParameter(pr, gl.LINK_STATUS)) throw new Error('link');
    gl.useProgram(pr);
    var buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    var al = gl.getAttribLocation(pr, 'a'); gl.enableVertexAttribArray(al); gl.vertexAttribPointer(al, 2, gl.FLOAT, false, 0, 0);
    var U = {};
    ['uC', 'uR', 'uAA', 'L', 'hub', 'b0', 'b1', 'track', 'glow', 'wid', 'fade', 'tint'].forEach(function (n) { U[n] = gl.getUniformLocation(pr, n); });
    var d = { gl: gl, canvas: canvas, dpr: 1, w: 0, h: 0 };
    d.resize = function (w, h, dpr) {
      d.dpr = dpr; d.w = w; d.h = h;
      var pw = Math.max(1, Math.round(w * dpr)), ph = Math.max(1, Math.round(h * dpr));
      if (canvas.width !== pw || canvas.height !== ph) { canvas.width = pw; canvas.height = ph; }
      gl.viewport(0, 0, pw, ph);
    };
    /* s: cx, cy, R in CSS pixels from the canvas top-left */
    d.draw = function (s) {
      var k = d.dpr;
      gl.uniform2f(U.uC, s.cx * k, (d.h - s.cy) * k);
      gl.uniform1f(U.uR, s.R * k);
      gl.uniform1f(U.uAA, 1.5 / Math.max(1, s.R * k));
      gl.uniform3f(U.L, s.L[0], s.L[1], s.L[2]);
      gl.uniform1f(U.hub, s.hub); gl.uniform1f(U.b0, s.b0); gl.uniform1f(U.b1, s.b1);
      gl.uniform1f(U.track, s.track); gl.uniform1f(U.glow, s.glow); gl.uniform1f(U.wid, s.wid || .3);
      gl.uniform1f(U.fade, s.fade);
      gl.uniform3f(U.tint, 1, .22, .37);
      gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    };
    d.lose = function () { var x = gl.getExtension('WEBGL_lose_context'); if (x) x.loseContext(); };
    return d;
  }

  /* ------------------------------------------------------------------
     Still layout (reduced motion): one still frame of the disc, nothing moves.
     ------------------------------------------------------------------ */
  var stillPainted = false;
  function paintStill() {
    if (stillPainted) return;
    var c = $('.still-gl');
    if (!c || !c.clientWidth) return;
    try {
      var d = makeDisc(c, true);
      if (!d) return;
      d.resize(c.clientWidth, c.clientHeight, Math.min(window.devicePixelRatio || 1, 2));
      var r = c.clientWidth / 2;
      d.draw({ cx: r, cy: r, R: r, hub: .5, b0: .54, b1: .62, track: -1, glow: .06, fade: 1, L: [Math.cos(-2.2) * 1.6, Math.sin(-2.2) * 1.6, 1.25] });
      c.parentNode.classList.add('gl-on');
      stillPainted = true;
      c.addEventListener('webglcontextlost', function () {
        c.parentNode.classList.remove('gl-on');
        stillPainted = false;
      }, { once: true });
    } catch (e) { /* the CSS disc stays */ }
  }

  if (!root.classList.contains('full')) {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', paintStill); else paintStill();
    reduceMQ.addEventListener && reduceMQ.addEventListener('change', function () { location.reload(); });
    restoreInitialHash();
    return;
  }

  /* ==================================================================
     THE SHOW
     ================================================================== */
  var theatre = $('.theatre');
  var press = $('.press', theatre), worldsL = $('.worlds', theatre), stage = $('.stage', theatre);
  var canvas = $('.gl', press), discCss = $('.disc-css', press), etch = $('.etch', press);
  var hub = $('.hub', press), hubA = $('.hub-a', hub), hubB = $('.hub-b', hub), laser = $('.laser', press);
  var hand = $('.hand', theatre);
  var worlds = $$('.world', worldsL);
  var heroCopy = $('.hero-copy'), heroSec = $('.act-hero'), tracksSec = $('.act-tracks');
  var tlHead = $('.tl-head');
  var items = $$('.tracks li');
  var thBridge = $('.th-bridge'), thRun = $('.th-run'), thIntro = $('.th-intro');
  var clBridge = $('.cl-bridge'), show = $('.show');
  var indexNav = $('.index'), indexBtns = $$('.index button');
  var motionBtn = $('.motion'), motionLabel = $('.motion-label');
  var stScreenLive = $('.st-live .st-screen', stage), stScreenMir = $('.st-mirror .st-screen', stage);
  var stNow = $('.st-now', stage);
  var NT = THEMES.length;
  var HOLD = .6; /* segments of scroll that halo is held whole before the first sweep (css .th-run adds the same) */

  /* build the eight records twice: the live orbit and its reflection in the lacquer floor */
  var recs = [], mrecs = [];
  function buildRecs(host, list, live) {
    THEMES.forEach(function (t, i) {
      var r = document.createElement('div'); r.className = 'rec'; r.style.setProperty('--i', i);
      var im = document.createElement('img'); im.src = 'img/vinyl-' + t.key + '-dark.webp'; im.alt = ''; im.width = 480; im.height = 480; im.loading = 'lazy'; im.decoding = 'async';
      var sh = document.createElement('i');
      r.appendChild(im); r.appendChild(sh);
      if (live) r.addEventListener('click', function () { bringForward(i); });
      host.appendChild(r);
      list.push({ el: r, shade: sh, z: -1 });
    });
  }
  buildRecs($('.st-live .st-records', stage), recs, true);
  buildRecs($('.st-mirror .st-records', stage), mrecs, false);

  var disc = null;
  try { disc = makeDisc(canvas, false); } catch (e) { disc = null; }
  if (!disc) press.classList.add('no-gl');
  canvas.addEventListener('webglcontextlost', function () {
    disc = null;
    press.classList.add('no-gl');
    lastDraw = '';
    kick();
  });

  /* ---------------- state ---------------- */
  var W = 0, H = 0, phone = false, G = null, M = null;
  var paused = savedPaused, still = false, snap = false, pageVisible = !document.hidden;
  var layoutStill = false, theatreVisible = true;
  var raf = 0, lastT = 0, introT0 = 0;
  var ang = -2.2, drift = 0, pointerAng = null, pointerAt = -1e9;
  var laserR = -1, trackF = -1;
  var curK = -2, hubK = -1, hubSrc = 'np-default-dark', hubBusy = 0;
  var spin = 135, spinTween = null, front = -1, shown = -1, stageReady = false;
  var lastDraw = '', lastScrollY = -1, scrollDirty = true;
  var samples = [];
  var lastOn = { press: null, worlds: null, stage: null };
  var worldCur = -1, worldShown = -1, roomRGB = ROSE;

  function setRoom(rgb) {
    if (rgb === roomRGB) return;
    roomRGB = rgb; root.style.setProperty('--room', rgb);
  }

  /* ---------------- geometry for every act ---------------- */
  function computeGeo() {
    var g = {};
    if (!phone) {
      var Rh = Math.max(.64 * H, .42 * W);
      g.hero = { cx: .67 * W, cy: .52 * H, R: Rh, hub: .37, b0: .45, b1: .52 };
      var P = Math.min(.78 * H, .40 * W, 720);
      var colR = Math.min(W, 1440) / 2 + Math.max(0, (W - 1440) / 2);
      g.deck = { cx: colR + Math.min(W, 1440) * .24, cy: .5 * H, R: P / 2, hub: .5, b0: .54, b1: .64 };
      var sd = Math.min(.74 * H, .48 * W, 760);
      g.sweep = { cx: .63 * W, cy: .52 * H, d: sd };
      var ad = Math.min(.36 * H, .25 * W, 440);
      var cw = Math.min(W, 1440), cl = (W - cw) / 2;
      g.stage = { cx: cl + cw * .68, cy: Math.max(80 + .8 * ad, .44 * H), d: ad, rx: .98, ry: .62, c: .3 };
      g.laserDeg = -60;
    } else {
      var R = Math.min(.66 * W, 290);
      g.hero = { cx: .5 * W, cy: 62 + .78 * R, R: R, hub: .46, b0: .5, b1: .56 };
      var Rd = Math.min(.46 * W, .24 * H, 200); /* as wide as the gutters allow, so the screen at the hub reads */
      g.deck = { cx: .5 * W, cy: 64 + Rd, R: Rd, hub: .6, b0: .64, b1: .72 };
      var sdp = Math.min(.84 * W, .46 * H);
      g.sweep = { cx: .5 * W, cy: 84 + sdp / 2, d: sdp };
      var adp = Math.min(.44 * W, .21 * H);
      g.stage = { cx: .5 * W, cy: 70 + .9 * adp, d: adp, rx: .74, ry: .62, c: .32 };
      g.laserDeg = 205;
    }
    g.stage.floorY = g.stage.cy + g.stage.d * (phone ? .9 : .84);
    return g;
  }

  function docTop(el) { return el.getBoundingClientRect().top + window.scrollY; }

  function measure() {
    root.classList.add('full');
    W = theatre.clientWidth; H = theatre.clientHeight; phone = W <= 860;
    G = computeGeo();
    var s = G.sweep, a = G.stage;
    theatre.style.setProperty('--sx', s.cx.toFixed(1) + 'px');
    theatre.style.setProperty('--sy', s.cy.toFixed(1) + 'px');
    theatre.style.setProperty('--sd', s.d.toFixed(1) + 'px');
    root.style.setProperty('--sx', s.cx.toFixed(1) + 'px');
    root.style.setProperty('--sy', s.cy.toFixed(1) + 'px');
    root.style.setProperty('--sd', s.d.toFixed(1) + 'px');
    stage.style.setProperty('--ax', a.cx.toFixed(1) + 'px');
    stage.style.setProperty('--ay', a.cy.toFixed(1) + 'px');
    stage.style.setProperty('--ad', a.d.toFixed(1) + 'px');
    stage.style.setProperty('--floorY', a.floorY.toFixed(1) + 'px');
    /* where the text sits so it never covers the screen */
    var h = G.hero, dk = G.deck;
    root.style.setProperty('--heroTop', Math.round(h.cy + h.R * .74) + 'px');
    root.style.setProperty('--pBot', Math.round(dk.cy + dk.R) + 'px');
    G.capTop = Math.round(dk.cy + dk.R + 34);
    root.style.setProperty('--capTop', G.capTop + 'px');
    root.style.setProperty('--introTop', Math.round(phone ? s.cy + s.d / 2 + 54 : .15 * H) + 'px');

    if (disc) disc.resize(W, H, Math.min(window.devicePixelRatio || 1, still ? 1 : (phone ? 1.5 : 1.25)));

    var y0 = window.scrollY;
    M = {};
    M.heroLen = Math.max(1, heroSec.offsetHeight * .85);
    M.items = items.map(function (li) { var t = li.getBoundingClientRect().top + y0; return [t, t + li.offsetHeight, $('.tr', li).offsetHeight]; });
    var bt = docTop(thBridge);
    M.bridgeStart = bt - H;
    M.bridgeEnd = bt + thBridge.offsetHeight - H;
    /* Hand over before the theme layer and bridge glow begin at t2 = .2. */
    M.trackEnd = M.bridgeStart + .2 * (M.bridgeEnd - M.bridgeStart);
    M.sweepLen = Math.max(1, thRun.offsetHeight);
    M.sweepEnd = M.bridgeEnd + M.sweepLen;
    M.b34Len = Math.max(1, clBridge.offsetHeight);
    M.showEnd = docTop(show) + show.offsetHeight;
    M.docMax = Math.max(1, document.documentElement.scrollHeight - window.innerHeight);
    /* The hero can scroll; only the pinned captions and controls must fit. */
    var captionH = Math.max.apply(Math, M.items.map(function (m) { return m[2]; }));
    var infoH = Math.max.apply(Math, worlds.map(function (w) { return $('.info', w).offsetHeight; }));
    var headerH = $('.bar').offsetHeight;
    layoutStill = phone ? G.capTop + captionH + 32 > H ||
      s.cy + s.d / 2 + 6 + indexNav.offsetHeight + infoH + 22 > H ||
      indexNav.offsetWidth + 32 > W :
      H < 560 || captionH + headerH + 32 > H ||
      infoH + clamp(.07 * H, 28, 72) + headerH + 16 > H ||
      indexNav.offsetHeight + 2 * (headerH + 16) > H;
    root.classList.toggle('full', !layoutStill);
    root.classList.toggle('calm', layoutStill);
    if (layoutStill) root.classList.remove('intro');
    motionBtn.hidden = layoutStill;
    if (layoutStill) paintStill();
    document.dispatchEvent(new Event('diskos-layout'));
    lastDraw = ''; scrollDirty = true;
  }

  /* ---------------- hub: each new screen opens from the centre out ---------------- */
  function setHub(name, immediate) {
    var src = 'img/' + name + '.webp';
    if (immediate) {
      if (name === hubSrc && hubA.getAttribute('src') === src &&
          hubB.getAttribute('src') === src && !hubB.classList.contains('in')) return;
      ++hubBusy; /* Cancel pending decodes and reveal timers before the bridge. */
      hubSrc = name;
      hubA.src = src; hubB.src = src;
      hubB.classList.remove('in');
      return;
    }
    if (name === hubSrc) return;
    hubSrc = name;
    var my = ++hubBusy;
    var pre = new Image(); pre.src = src;
    var go = function () {
      if (my !== hubBusy) return;
      hubB.classList.remove('in');
      hubB.src = src;
      void hubB.offsetWidth;
      hubB.classList.add('in');
      setTimeout(function () { if (my === hubBusy) { hubA.src = src; hubB.classList.remove('in'); } }, 950);
    };
    (pre.decode ? pre.decode() : Promise.resolve()).then(go, go);
  }

  /* ---------------- act 4: which theme is on the screen ---------------- */
  var swapState = false, showToken = 0;
  function showTheme(i) {
    if (i === shown) return;
    shown = i;
    var t = THEMES[i];
    var src = 'img/np-' + t.key + '-dark.webp';
    var my = ++showToken;
    var pre = new Image(); pre.src = src;
    var go = function () {
      if (my !== showToken) return;
      var set = $$(swapState ? '.g-a' : '.g-b', stage);
      set.forEach(function (im) { im.src = src; });
      swapState = !swapState;
      stage.classList.toggle('swap', swapState);
      $('.t', stNow).textContent = t.name; $('.s', stNow).textContent = t.song; $('.a', stNow).textContent = t.artist;
      if (stageReady) setRoom(t.rgb);
    };
    (pre.decode ? pre.decode() : Promise.resolve()).then(go, go);
  }
  function norm(a) { return ((a % 360) + 360) % 360; }
  function frontIndex() {
    var best = 0, bd = 999;
    for (var i = 0; i < NT; i++) { var d = Math.abs(norm(spin + i * 45 - 90)); d = Math.min(d, 360 - d); if (d < bd) { bd = d; best = i; } }
    return best;
  }
  function bringForward(i) {
    if (!stageReady) return;
    var target = 90 - i * 45, delta = norm(target - spin);
    if (delta > 180) delta -= 360;
    showTheme(i);
    spinTween = { from: spin, to: spin + delta, t0: performance.now(), dur: 1300 };
    kick();
  }

  /* ---------------- the frame ---------------- */
  function running() { return !paused && !still && pageVisible && theatreVisible && !layoutStill; }

  function frame(now) {
    raf = 0;
    var dt = lastT ? Math.min(100, now - lastT) : 16; lastT = now;
    var y = window.scrollY;
    theatreVisible = y < M.showEnd;
    root.classList.toggle('motion-idle', !pageVisible || !theatreVisible || layoutStill);
    if (!pageVisible || !theatreVisible || layoutStill) { lastT = 0; return; }
    if (y !== lastScrollY) { lastScrollY = y; scrollDirty = true; }
    var keep = false;

    /* progress through the acts */
    var t1 = c01(y / M.heroLen), e1 = inOut(t1);
    var t2 = c01((y - M.bridgeStart) / (M.bridgeEnd - M.bridgeStart));
    var q = c01((y - M.bridgeEnd) / M.sweepLen) * (NT - 1 + HOLD), p = Math.max(0, q - HOLD);
    var t3 = c01((y - M.sweepEnd) / M.b34Len);

    var pressOn = t2 < 1, worldsOn = t2 > .2 && t3 < .56, stageOn = t3 > 0;
    if (pressOn !== lastOn.press) { press.classList.toggle('on', pressOn); lastOn.press = pressOn; }
    if (worldsOn !== lastOn.worlds) { worldsL.classList.toggle('on', worldsOn); lastOn.worlds = worldsOn; }
    if (stageOn !== lastOn.stage) { stage.classList.toggle('on', stageOn); lastOn.stage = stageOn; }

    /* page progress around the pause control */
    if (scrollDirty) motionBtn.style.setProperty('--prog', (106.8 * (1 - c01(y / M.docMax))).toFixed(1));

    /* ================= ACTS 1 + 2: the pressed disc ================= */
    if (pressOn) {
      var intro = introT0 ? c01((now - introT0) / 1600) : 1;
      if (intro < 1) keep = true;
      var A = G.hero, B = G.deck;
      var cx = lerp(A.cx, B.cx, e1), cy = lerp(A.cy, B.cy, e1), R = lerp(A.R, B.R, e1);
      var hr = lerp(A.hub, B.hub, e1), r0 = lerp(A.b0, B.b0, e1), r1 = lerp(A.b1, B.b1, e1);
      var hubPx = R * hr;
      if (t2 > 0) {
        var e2 = inOut(t2), sw = G.sweep;
        hubPx = lerp(hubPx, sw.d / 2, e2);
        cx = lerp(cx, sw.cx, e2); cy = lerp(cy, sw.cy, e2);
        R = lerp(R, Math.hypot(W, H) * 1.7, Math.pow(t2, 2.1));
        hr = hubPx / R; r0 = hr * (B.b0 / B.hub); r1 = Math.min(.9, hr * (B.b1 / B.hub));
      }
      var fade = outCubic(intro) * (1 - sstep(.28, .82, t2));

      /* active track: whichever item holds the reading line */
      var k = -1;
      if (t1 > .55) {
        if (!phone) {
          var line = y + H * .5;
          for (var j = 0; j < M.items.length; j++) if (line >= M.items[j][0] && line < M.items[j][1]) { k = j; break; }
          if (k < 0 && M.items.length && line >= M.items[M.items.length - 1][1]) k = M.items.length - 1;
        } else {
          /* phones: each caption holds under the disc; it shows only while it is parked there */
          for (var j2 = 0; j2 < M.items.length; j2++) {
            var T = M.items[j2][0] - y, lh = M.items[j2][1] - M.items[j2][0];
            if (T <= G.capTop + 40 && T > G.capTop + M.items[j2][2] - lh + 24) { k = j2; break; }
          }
        }
      }
      var leftTracks = phone ? t2 > .02 : y >= M.trackEnd;
      if (leftTracks) k = -1;
      /* The hero's links remain readable and focusable as they scroll away. */
      heroCopy.style.opacity = '1';
      if (k !== curK) {
        items.forEach(function (li, j) { li.classList.toggle('on', j === k); });
        curK = k;
      }
      if (phone) {
        var firstT = M.items.length ? M.items[0][0] - y : 1e9, lastB = M.items.length ? M.items[M.items.length - 1][1] - y : -1e9;
        root.style.setProperty('--headOp', firstT > G.capTop + 60 ? '1' : '0');
        root.style.setProperty('--alsoOp', (lastB < G.capTop + 140 && t2 < .08) ? '1' : '0');
      }
      /* the screen keeps the last feature while the reading line crosses the gap to the next one;
         it shows the default only before the first feature and at the hand-over to themes */
      var beforeFirst = t1 <= .55 || !M.items.length ||
        (phone ? M.items[0][0] - y > G.capTop + 40 : y + H * .5 < M.items[0][0]);
      if (k >= 0) hubK = k;
      else if (beforeFirst || leftTracks) hubK = -1;
      var want = hubK >= 0 ? items[hubK].dataset.img : 'np-default-dark';
      setHub(want, !phone && leftTracks);

      /* the light: follows the pointer, otherwise drifts; scrolling turns it */
      if (disc && running() && intro >= 1) { drift += dt * .00016; keep = true; }
      var target = -2.2 + drift + y * (phone ? .0035 : .0011) + lerp(0, 1.1, e1) + (k >= 0 ? k * .18 : 0);
      if (pointerAng !== null && now - pointerAt < 2600) {
        var pa = pointerAng; while (pa - target > Math.PI) pa -= 2 * Math.PI; while (pa - target < -Math.PI) pa += 2 * Math.PI;
        target = pa;
      }
      target += (1 - outCubic(intro)) * -1.4;
      while (target - ang > Math.PI) target -= 2 * Math.PI;
      while (target - ang < -Math.PI) target += 2 * Math.PI;
      var da = target - ang;
      ang += da * Math.min(1, dt * .006);
      if (disc && Math.abs(da) > .0008) keep = true;
      var dist = lerp(2.4, 1.55, e1), hz = lerp(.95, 1.3, e1);

      /* laser and the band it lights */
      var lOn = k >= 0 && t2 < .1;
      var lt = k >= 0 ? R * (r1 + (k + .5) / 8 * (.985 - r1)) : R * (r1 + .5 * (.985 - r1));
      if (laserR < 0) laserR = lt;
      laserR += (lt - laserR) * Math.min(1, dt * .009);
      trackF += ((k >= 0 ? k : -1) - trackF) * Math.min(1, dt * .012);
      if (Math.abs(lt - laserR) > .3 || Math.abs((k >= 0 ? k : -1) - trackF) > .01) keep = true;
      laser.classList.toggle('on', lOn);
      var th = G.laserDeg * Math.PI / 180;
      laser.style.transform = 'translate3d(' + (cx + Math.sin(th) * laserR).toFixed(1) + 'px,' + (cy - Math.cos(th) * laserR).toFixed(1) + 'px,0)';

      /* place the screen, the etching and the CSS stand-in */
      var hd = hubPx * 2;
      hub.style.width = hub.style.height = hd.toFixed(1) + 'px';
      hub.style.transform = 'translate3d(' + (cx - hubPx).toFixed(1) + 'px,' + (cy - hubPx).toFixed(1) + 'px,0)';
      hub.style.setProperty('--hglow', (hd * (.16 + .5 * sstep(.2, .9, t2))).toFixed(0) + 'px');
      var mid = R * (r0 + r1) / 2, S = mid * 1000 / 247;
      etch.style.width = etch.style.height = S.toFixed(1) + 'px';
      etch.style.transform = 'translate3d(' + (cx - S / 2).toFixed(1) + 'px,' + (cy - S / 2).toFixed(1) + 'px,0)';
      etch.style.opacity = ((1 - sstep(0, .3, t2)) * (phone ? 1 - e1 : 1 - .45 * e1) * outCubic(intro)).toFixed(3);
      if (!disc || snap) {
        discCss.style.width = discCss.style.height = (R * 2).toFixed(1) + 'px';
        discCss.style.transform = 'translate3d(' + (cx - R).toFixed(1) + 'px,' + (cy - R).toFixed(1) + 'px,0)';
        discCss.style.opacity = fade.toFixed(3);
      }
      press.style.setProperty('--scrim', (1 - sstep(0, .6, t2)).toFixed(3));

      if (disc && !snap) {
        var Lx = Math.cos(ang) * dist, Ly = Math.sin(ang) * dist;
        var key = [cx, cy, R, hr, r0, r1, trackF, fade, Lx, Ly, hz].map(function (v) { return v.toFixed(3); }).join(',');
        if (key !== lastDraw) {
          disc.draw({ cx: cx, cy: cy, R: R, hub: hr, b0: r0, b1: r1, track: trackF, glow: lerp(.05, .08, e1), fade: fade, L: [Lx, Ly, hz], wid: .3 });
          lastDraw = key;
        }
      }
    }

    /* ================= ACT 3: eight worlds ================= */
    var introOp = 1 - c01((q - .04) / .3);
    if (thIntro) thIntro.style.opacity = t2 > 0 ? introOp.toFixed(3) : '1';
    indexNav.classList.toggle('on', t2 > .92 && t3 < .3);
    if (worldsOn && scrollDirty) {
      worldsL.style.opacity = sstep(.2, .7, t2).toFixed(3);
      worldsL.style.setProperty('--hero', (t2 < 1 ? 1 : introOp).toFixed(3));
      var i = Math.min(Math.floor(p), NT - 2), f = p - i;
      var s = c01((f - .14) / .72);
      if (t3 > 0) { i = NT - 2; s = 1; }
      if (i !== worldCur) {
        worlds.forEach(function (w, n) { w.classList.toggle('is-cur', n === i); w.classList.toggle('is-next', n === i + 1); });
        worldCur = i;
        /* the images two worlds ahead start loading now */
        [i + 2, i + 3].forEach(function (n) { var im = worlds[n] && $('.disc', worlds[n]); if (im && im.loading === 'lazy') im.loading = 'eager'; });
      }
      worlds[i + 1].style.setProperty('--sweep', (s >= .999 ? 362 : s * 360).toFixed(2) + 'deg');
      /* each theme's words leave before the edge reaches them, and the next ones arrive after it has passed */
      worlds[i].style.setProperty('--infoOp', c01(1 - (s - .1) * 2.8).toFixed(3));
      worlds[i + 1].style.setProperty('--infoOp', c01((s - .55) * 2.4).toFixed(3));
      worldsL.classList.toggle('bridging', t2 < 1);
      var sh = s >= .5 ? i + 1 : i;
      if (sh !== worldShown) {
        indexBtns.forEach(function (b, n) { b.setAttribute('aria-current', String(n === sh)); });
        worldShown = sh;
      }
      if (!stageOn) setRoom(t2 > .5 ? THEMES[sh].rgb : ROSE);
      setHand(G.sweep.cx, G.sweep.cy, G.sweep.d / 2, s * 360, s > 0 && s < 1 && t3 === 0);
    } else if (!worldsOn && t3 === 0) {
      hand.classList.remove('on');
      if (t2 <= .5) setRoom(ROSE);
    }

    /* ================= ACT 4: the lit stage ================= */
    if (stageOn) {
      var wa = inOut(c01(t3 / .55)) * 360;
      var masked = t3 < .55;
      stage.classList.toggle('masked', masked);
      if (masked) { stage.style.setProperty('--wa', wa.toFixed(2) + 'deg'); setHand(G.sweep.cx, G.sweep.cy, G.sweep.d / 2, wa, wa > 0 && wa < 360); }
      else hand.classList.remove('on');

      var tb = inOut(c01((t3 - .42) / .58));
      var St = G.stage, Sw = G.sweep;
      var scx = lerp(Sw.cx, St.cx, tb), scy = lerp(Sw.cy, St.cy, tb), sd = lerp(Sw.d, St.d, tb);
      var lit = sstep(.15, 1, tb);
      stage.style.setProperty('--lit', lit.toFixed(3));
      stage.style.setProperty('--nowOp', sstep(.75, 1, tb).toFixed(3));
      placeScreen(stScreenLive, scx, scy, sd);
      placeScreen(stScreenMir, scx, scy, sd);

      var wasReady = stageReady;
      stageReady = t3 >= 1;
      if (!stageReady && shown !== 7) { spin = 135; showTheme(7); }
      if (stageReady && !wasReady) setRoom(THEMES[shown >= 0 ? shown : 7].rgb);
      if (!stageReady && !masked) setRoom(THEMES[7].rgb);

      /* orbit: the records fly out from behind the screen, then keep turning */
      if (spinTween) {
        var q = c01((now - spinTween.t0) / spinTween.dur), ee = 1 - Math.pow(2, -10 * q);
        spin = spinTween.from + (spinTween.to - spinTween.from) * (q === 1 ? 1 : ee);
        if (q === 1) spinTween = null;
        keep = true;
      } else if (stageReady && running()) {
        spin += dt * 360 / 72000; keep = true;
        var fi = frontIndex();
        if (fi !== front) { front = fi; showTheme(fi); }
      }
      var rx = St.rx * St.d, ry = St.ry * St.d, c = St.c * St.d;
      for (var n = 0; n < NT; n++) {
        var qn = outCubic(c01((tb - .12 - n * .045) / .5));
        var aa = (spin + n * 45) * Math.PI / 180, sn = Math.sin(aa);
        var x = scx + Math.cos(aa) * rx * qn, yy = scy + sn * ry * qn;
        var sc = (.62 + (sn + 1) * .27) * (.25 + .75 * qn);
        var tf = 'translate3d(' + (x - c / 2).toFixed(1) + 'px,' + (yy - c / 2).toFixed(1) + 'px,0) scale(' + sc.toFixed(3) + ')';
        var z = sn > 0 ? 5 : 1;
        [recs[n], mrecs[n]].forEach(function (rr) {
          rr.el.style.width = rr.el.style.height = c.toFixed(1) + 'px';
          rr.el.style.transform = tf;
          rr.el.style.opacity = qn.toFixed(3);
          rr.shade.style.opacity = (.62 - (sn + 1) * .3).toFixed(3);
          if (rr.z !== z) { rr.el.style.zIndex = z; rr.z = z; }
        });
      }
    } else {
      stageReady = false;
      spinTween = null;
    }

    scrollDirty = false;

    /* a device that cannot keep up gets stills: first the autonomous motion stops, then the disc becomes a picture */
    if (running() && (pressOn || stageReady) && dt < 200) {
      samples.push(dt);
      if (samples.length >= 90) {
        var sorted = samples.slice().sort(function (a, b) { return a - b; });
        var med = sorted[45];
        samples = [];
        if (med > 30) goStill();
      }
    }

    if (keep && pageVisible) raf = requestAnimationFrame(frame);
    else lastT = 0;
  }

  function placeScreen(el, x, y, d) {
    el.style.width = el.style.height = d.toFixed(1) + 'px';
    el.style.transform = 'translate3d(' + (x - d / 2).toFixed(1) + 'px,' + (y - d / 2).toFixed(1) + 'px,0)';
  }
  function setHand(x, y, r, deg, on) {
    hand.style.setProperty('--hx', x.toFixed(1) + 'px');
    hand.style.setProperty('--hy', y.toFixed(1) + 'px');
    hand.style.setProperty('--hr', r.toFixed(1) + 'px');
    hand.style.setProperty('--ha', deg.toFixed(2) + 'deg');
    hand.classList.toggle('on', !!on);
  }

  function goStill() {
    if (still) return;
    still = true; root.classList.add('still-mode');
    if (disc) disc.resize(W, H, 1);
    lastDraw = '';
    /* if even scroll-driven frames are slow, swap the live disc for one rendered picture of it */
    var slow = [], t0 = 0;
    function watch(now) {
      if (t0) slow.push(now - t0);
      t0 = now;
      if (slow.length < 40) { requestAnimationFrame(watch); return; }
      var s = slow.sort(function (a, b) { return a - b; })[20];
      if (s > 45) makeSnap();
    }
    requestAnimationFrame(watch);
  }
  function makeSnap() {
    if (snap || !disc) return;
    try {
      var c = document.createElement('canvas');
      var d = makeDisc(c, true);
      if (!d) return;
      d.resize(700, 700, 1);
      d.draw({ cx: 350, cy: 350, R: 350, hub: .5, b0: .54, b1: .64, track: -1, glow: .06, fade: 1, L: [Math.cos(-1.1) * 1.55, Math.sin(-1.1) * 1.55, 1.3] });
      press.style.setProperty('--snap', 'url(' + c.toDataURL('image/png') + ')');
      d.lose();
      snap = true; press.classList.add('snap'); disc.lose(); disc = null;
      kick();
    } catch (e) { /* keep the live disc */ }
  }

  function kick() { if (!raf) { raf = requestAnimationFrame(frame); } }

  /* ---------------- input ---------------- */
  window.addEventListener('scroll', function () { scrollDirty = true; kick(); }, { passive: true });
  var rzT = 0;
  window.addEventListener('resize', function () {
    clearTimeout(rzT);
    rzT = setTimeout(function () {
      measure(); kick();
    }, 120);
  });
  window.addEventListener('pointermove', function (e) {
    if (e.pointerType !== 'mouse' || !fineMQ.matches || paused || !lastOn.press) return;
    var y = window.scrollY, t1 = c01(y / M.heroLen), e1 = inOut(t1);
    var cx = lerp(G.hero.cx, G.deck.cx, e1), cy = lerp(G.hero.cy, G.deck.cy, e1);
    pointerAng = Math.atan2(-(e.clientY - cy), e.clientX - cx);
    pointerAt = performance.now();
    kick();
  }, { passive: true });
  document.addEventListener('visibilitychange', function () {
    pageVisible = !document.hidden;
    root.classList.toggle('motion-idle', !pageVisible || layoutStill || !theatreVisible);
    if (raf) cancelAnimationFrame(raf);
    raf = 0; lastT = 0;
    if (pageVisible) kick();
  });
  reduceMQ.addEventListener && reduceMQ.addEventListener('change', function () { location.reload(); });

  motionBtn.setAttribute('aria-pressed', String(paused));
  motionLabel.textContent = paused ? 'Resume motion' : 'Pause motion';
  motionBtn.addEventListener('click', function () {
    paused = !paused;
    try { localStorage.setItem('diskos-motion-paused', paused ? '1' : '0'); } catch (e) { /* keep this page's choice */ }
    if (paused) { spinTween = null; introT0 = 0; root.classList.remove('intro'); }
    motionBtn.setAttribute('aria-pressed', String(paused));
    motionLabel.textContent = paused ? 'Resume motion' : 'Pause motion';
    root.classList.toggle('paused', paused);
    kick();
  });

  /* theme index: land just after the sweep into theme k has finished */
  function themeTop(k) {
    return M.bridgeEnd + M.sweepLen * (k === 0 ? HOLD * .7 : HOLD + k - 1 + .9) / (NT - 1 + HOLD);
  }
  indexBtns.forEach(function (b) {
    b.addEventListener('click', function () {
      var k = +b.dataset.i;
      var y = themeTop(k);
      window.scrollTo({ top: Math.round(y), behavior: 'smooth' });
    });
  });

  /* in-page links land on the frame that tells that part best */
  function jump(hash) {
    if (layoutStill) return false;
    var y = null, focus = null;
    if (hash === '#top') { y = 0; focus = $('#hero-title'); }
    else if (hash === '#tracklist') { y = docTop(tracksSec) + (phone ? H * .1 : H * .06); focus = $('#tl-title'); }
    else if (hash === '#themes') { y = M.bridgeEnd; focus = $('#th-title'); }
    else if (hash === '#install') {
      focus = $('#close-title');
      y = Math.min(M.showEnd - H, docTop(focus) - $('.bar').offsetHeight - 16);
    }
    if (y === null) return false;
    window.scrollTo({ top: Math.round(y), behavior: 'smooth' });
    if (focus) { focus.setAttribute('tabindex', '-1'); focus.focus({ preventScroll: true }); }
    if (history.replaceState) history.replaceState(null, '', hash);
    return true;
  }
  $$('a[href^="#"]').forEach(function (a) {
    a.addEventListener('click', function (e) {
      var h = a.getAttribute('href');
      if (h === '#main') return;
      if (jump(h)) e.preventDefault();
    });
  });

  /* ---------------- go ---------------- */
  measure();
  introT0 = paused || layoutStill ? 0 : performance.now();
  setTimeout(function () { root.classList.remove('intro'); }, 2600);
  /* fonts and late images can move the text: measure again once they settle */
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(function () { measure(); kick(); });
  window.addEventListener('load', function () { measure(); kick(); });
  restoreInitialHash();
  kick();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start, { once: true });
  else start();
})();

/* phone: the header Get diskOS appears once the hero's own button has scrolled out above the viewport (css .cta-on) */
(function () {
  'use strict';
  function startHeader() {
  var bar = document.querySelector('.bar'), hero = document.querySelector('.act-hero .actions .btn');
  if (!bar || !hero) return;
  if (!('IntersectionObserver' in window)) { bar.classList.add('cta-on'); return; }
  var bottom = 0, measureRaf = 0;
  function update() { bar.classList.toggle('cta-on', window.scrollY >= bottom - 56); }
  function measureHero() {
    measureRaf = 0;
    bottom = hero.getBoundingClientRect().bottom + window.scrollY;
    update();
  }
  function scheduleMeasure() { if (!measureRaf) measureRaf = requestAnimationFrame(measureHero); }
  /* A jump can skip the intersecting state; scroll updates use the cached document position. */
  new IntersectionObserver(scheduleMeasure, { rootMargin: '-56px 0px 0px 0px' }).observe(hero);
  window.addEventListener('scroll', update, { passive: true });
  window.addEventListener('resize', scheduleMeasure);
  window.addEventListener('load', scheduleMeasure);
  document.addEventListener('diskos-layout', scheduleMeasure);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(scheduleMeasure);
  measureHero();
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', startHeader, { once: true });
  else startHeader();
})();
