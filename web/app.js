// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 diskOS contributors

(function () {
  'use strict';

  // State
  let authToken = localStorage.getItem('diskos_auth_token') || '';
  let curPath = '';
  let isSeeking = false;
  let isVolChanging = false;
  let previousVol = 40;
  let pollTimer = null;
  let ws = null;
  let wsReconnectTimer = null;
  let curDurationMs = 0;
  let currentTrackPath = '';
  let currentPlayMode = 0;
  let queueData = [];
  let currentDirectoryFiles = [];

  // Theme & Preferences State
  let vinylSpinEnabled = localStorage.getItem('diskos_vinyl_spin') !== 'false';
  let activeThemeColor = localStorage.getItem('diskos_theme_color') || '#ff5500';

  // DOM Elements - Auth & Modals
  const authModal = document.getElementById('auth-modal');
  const authForm = document.getElementById('auth-form');
  const authInput = document.getElementById('auth-input');
  const authError = document.getElementById('auth-error');
  const authInputWrap = authForm ? authForm.querySelector('.input-wrap') : null;
  const btnLogout = document.getElementById('btn-logout');

  const confirmModal = document.getElementById('confirm-modal');
  const confirmTitle = document.getElementById('confirm-title');
  const confirmDesc = document.getElementById('confirm-desc');
  const confirmCancel = document.getElementById('confirm-cancel');
  const confirmOk = document.getElementById('confirm-ok');
  let confirmCallback = null;

  const newFolderModal = document.getElementById('new-folder-modal');
  const newFolderForm = document.getElementById('new-folder-form');
  const newFolderInput = document.getElementById('new-folder-input');
  const newFolderInputWrap = document.getElementById('new-folder-input-wrap');
  const newFolderDest = document.getElementById('new-folder-dest');
  const newFolderCancel = document.getElementById('new-folder-cancel');
  const btnNewFolder = document.getElementById('btn-new-folder');

  const toastContainer = document.getElementById('toast-container');

  // DOM Elements - Header & Tabs
  const connStatus = document.getElementById('conn-status');
  const headerBattery = document.getElementById('bat-text');
  const tabsSlider = document.getElementById('tabs-slider');
  const navTabs = document.querySelectorAll('.nav-tab');

  // DOM Elements - Player
  const vinylTiltWrapper = document.getElementById('vinyl-tilt-wrapper');
  const artDisc = document.getElementById('art-disc');
  const trackArt = document.getElementById('track-art');
  const trackTitle = document.getElementById('track-title');
  const trackArtist = document.getElementById('track-artist');
  const trackAlbum = document.getElementById('track-album');
  const badgeFormat = document.getElementById('badge-format');
  const badgeRate = document.getElementById('badge-rate');
  const badgeBits = document.getElementById('badge-bits');
  const seekBar = document.getElementById('seek-bar');
  const timePos = document.getElementById('time-pos');
  const timeDur = document.getElementById('time-dur');
  const btnPlay = document.getElementById('btn-play');
  const btnPrev = document.getElementById('btn-prev');
  const btnNext = document.getElementById('btn-next');
  const btnShuffle = document.getElementById('btn-shuffle');
  const btnRepeat = document.getElementById('btn-repeat');
  const iconPlay = document.getElementById('icon-play');
  const iconPause = document.getElementById('icon-pause');
  const btnMute = document.getElementById('btn-mute');
  const iconVol = document.getElementById('icon-vol');
  const iconVolMute = document.getElementById('icon-vol-mute');
  const volBar = document.getElementById('vol-bar');
  const volVal = document.getElementById('vol-val');

  // DOM Elements - Queue
  const queueCount = document.getElementById('queue-count');
  const queueList = document.getElementById('queue-list');
  const queueSearch = document.getElementById('queue-search');
  const queueSearchClear = document.getElementById('queue-search-clear');
  const btnRefreshQueue = document.getElementById('btn-refresh-queue');
  const btnClearQueue = document.getElementById('btn-clear-queue');

  // DOM Elements - Files
  const filesBreadcrumb = document.getElementById('files-breadcrumb');
  const fileSearch = document.getElementById('file-search');
  const fileSearchClear = document.getElementById('file-search-clear');
  const fileList = document.getElementById('file-list');
  const btnRefreshFiles = document.getElementById('btn-refresh-files');
  const dropZone = document.getElementById('drop-zone');
  const fileInput = document.getElementById('file-input');
  const browseTrigger = document.getElementById('browse-trigger');
  const uploadStatus = document.getElementById('upload-status');
  const uploadFilename = document.getElementById('upload-filename');
  const uploadPct = document.getElementById('upload-pct');
  const uploadFill = document.getElementById('upload-fill');

  // DOM Elements - Settings
  const modePills = document.querySelectorAll('.mode-pill');
  const settingSleep = document.getElementById('setting-sleep');
  const settingBrightness = document.getElementById('setting-brightness');
  const brightnessVal = document.getElementById('brightness-val');
  const colorDots = document.querySelectorAll('.color-dot');
  const settingVinylSpin = document.getElementById('setting-vinyl-spin');
  const formChangePin = document.getElementById('form-change-pin');
  const settingNewPin = document.getElementById('setting-new-pin');
  const btnRestartUi = document.getElementById('btn-restart-ui');
  const btnRebootDev = document.getElementById('btn-reboot-dev');

  // DOM Elements - System
  const sysBatPct = document.getElementById('sys-bat-pct');
  const sysBatVolt = document.getElementById('sys-bat-volt');
  const sysCharging = document.getElementById('sys-charging');
  const sysSdFree = document.getElementById('sys-sd-free');
  const sysSdTotal = document.getElementById('sys-sd-total');
  const sysStorageFill = document.getElementById('sys-storage-fill');
  const btnRescanLib = document.getElementById('btn-rescan-lib');

  // -------------------------------------------------------------
  // Theme Accent Palette Initialization
  // -------------------------------------------------------------
  function applyTheme(color) {
    activeThemeColor = color;
    localStorage.setItem('diskos_theme_color', color);
    document.documentElement.style.setProperty('--accent-color', color);

    // Generate soft glow RGBA
    let r = 255, g = 85, b = 0;
    if (color.startsWith('#') && color.length === 7) {
      r = parseInt(color.slice(1, 3), 16);
      g = parseInt(color.slice(3, 5), 16);
      b = parseInt(color.slice(5, 7), 16);
    }
    document.documentElement.style.setProperty('--accent-glow', `rgba(${r}, ${g}, ${b}, 0.35)`);
    document.documentElement.style.setProperty('--accent-soft', `rgba(${r}, ${g}, ${b}, 0.12)`);

    colorDots.forEach(dot => {
      dot.classList.toggle('active', dot.dataset.color.toLowerCase() === color.toLowerCase());
    });
  }

  colorDots.forEach(dot => {
    dot.addEventListener('click', () => {
      applyTheme(dot.dataset.color);
      showToast(`Theme updated to ${dot.title || 'accent'}`, 'success');
    });
  });
  applyTheme(activeThemeColor);

  // Vinyl Spin Preference
  if (settingVinylSpin) {
    settingVinylSpin.checked = vinylSpinEnabled;
    artDisc.classList.toggle('spin-disabled', !vinylSpinEnabled);
    settingVinylSpin.addEventListener('change', () => {
      vinylSpinEnabled = settingVinylSpin.checked;
      localStorage.setItem('diskos_vinyl_spin', vinylSpinEnabled ? 'true' : 'false');
      artDisc.classList.toggle('spin-disabled', !vinylSpinEnabled);
      showToast(`Turntable rotation ${vinylSpinEnabled ? 'enabled' : 'disabled'}`);
    });
  }

  // -------------------------------------------------------------
  // Toast Notifications (22-toast)
  // -------------------------------------------------------------
  function showToast(message, type = 'info', title = '') {
    if (!toastContainer) return;

    const toast = document.createElement('div');
    toast.className = `t-toast ${type}`;

    let iconSvg = '';
    if (type === 'success') {
      iconSvg = '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"/></svg>';
    } else if (type === 'danger') {
      iconSvg = '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z"/></svg>';
    } else {
      iconSvg = '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor"><path d="M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm1 15h-2v-6h2v6zm0-8h-2V7h2v2z"/></svg>';
    }

    toast.innerHTML = `
      <div class="toast-icon" aria-hidden="true">${iconSvg}</div>
      <div class="toast-content">
        ${title ? `<div class="toast-title">${title}</div>` : ''}
        <div class="toast-msg">${message}</div>
      </div>
    `;

    toastContainer.appendChild(toast);
    void toast.offsetWidth; // force reflow for smooth open transition
    toast.classList.add('is-open');

    const closeTimer = setTimeout(() => {
      toast.classList.remove('is-open');
      setTimeout(() => {
        if (toast.parentNode) toast.parentNode.removeChild(toast);
      }, 250);
    }, 3200);

    toast.addEventListener('click', () => {
      clearTimeout(closeTimer);
      toast.classList.remove('is-open');
      setTimeout(() => {
        if (toast.parentNode) toast.parentNode.removeChild(toast);
      }, 250);
    });
  }

  // -------------------------------------------------------------
  // Modals (06-modal & 12-error-state-shake)
  // -------------------------------------------------------------
  function openModal(modalEl) {
    if (!modalEl) return;
    modalEl.classList.remove('hidden', 'is-closing');
    void modalEl.offsetWidth; // force reflow
    modalEl.classList.add('is-open');
  }

  function closeModal(modalEl) {
    if (!modalEl) return;
    modalEl.classList.remove('is-open');
    modalEl.classList.add('is-closing');
    setTimeout(() => {
      modalEl.classList.remove('is-closing');
      modalEl.classList.add('hidden');
    }, 150);
  }

  function showConfirmModal(opts) {
    confirmTitle.textContent = opts.title || 'Confirm Action';
    confirmDesc.textContent = opts.desc || 'Are you sure you want to proceed?';
    confirmOk.textContent = opts.okText || 'Confirm';
    confirmOk.className = opts.isDanger ? 'btn-danger' : 'btn-primary-block';
    confirmCallback = opts.onConfirm || null;
    openModal(confirmModal);
  }

  if (confirmCancel) confirmCancel.addEventListener('click', () => closeModal(confirmModal));
  if (confirmOk) {
    confirmOk.addEventListener('click', () => {
      closeModal(confirmModal);
      if (confirmCallback) confirmCallback();
    });
  }

  function triggerErrorShake(wrapEl) {
    if (!wrapEl) return;
    wrapEl.classList.add('is-error');
    wrapEl.classList.remove('is-shaking');
    void wrapEl.offsetWidth; // force reflow
    wrapEl.classList.add('is-shaking');

    setTimeout(() => {
      wrapEl.classList.remove('is-shaking');
    }, 280);

    if (wrapEl._revertTimer) clearTimeout(wrapEl._revertTimer);
    wrapEl._revertTimer = setTimeout(() => {
      wrapEl.classList.remove('is-error');
    }, 3000);
  }

  function handleUnauthorized() {
    authToken = '';
    localStorage.removeItem('diskos_auth_token');
    disconnectWebSocket();
    connStatus.classList.remove('online');
    connStatus.querySelector('.indicator-text').textContent = 'Locked';
    showAuthModal();
  }

  function showAuthModal() {
    authError.classList.add('hidden');
    openModal(authModal);
    setTimeout(() => {
      authInput.focus();
    }, 100);
  }

  function hideAuthModal() {
    closeModal(authModal);
    authError.classList.add('hidden');
  }

  // -------------------------------------------------------------
  // API Fetch Helper
  // -------------------------------------------------------------
  async function apiFetch(url, options = {}) {
    if (!authToken && url !== '/api/auth') {
      handleUnauthorized();
      throw new Error('Unauthorized');
    }
    options.headers = options.headers || {};
    if (authToken) {
      options.headers['Authorization'] = 'Bearer ' + authToken;
    }
    const resp = await fetch(url, options);
    if (resp.status === 401) {
      handleUnauthorized();
      throw new Error('Unauthorized');
    }
    return resp;
  }

  // Auth Form Submit
  if (authForm) {
    authForm.addEventListener('submit', async (e) => {
      e.preventDefault();
      const pw = authInput.value.trim();
      if (!pw) {
        triggerErrorShake(authInputWrap);
        authError.textContent = 'Please enter a PIN or password.';
        authError.classList.remove('hidden');
        authInput.focus();
        return;
      }

      const submitBtn = document.getElementById('auth-submit');
      const origText = submitBtn ? submitBtn.textContent : 'Unlock Device';
      if (submitBtn) {
        submitBtn.disabled = true;
        submitBtn.textContent = 'Verifying...';
      }

      try {
        const resp = await fetch('/api/auth', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ password: pw })
        });
        const data = await resp.json();
        if (data.ok && data.token) {
          authToken = data.token;
          localStorage.setItem('diskos_auth_token', authToken);
          connStatus.classList.add('online');
          connStatus.querySelector('.indicator-text').textContent = 'Online';
          hideAuthModal();
          connectWebSocket();
          loadFiles(curPath);
          loadQueue();
          loadSettings();
          loadSystem();
          showToast('Unlocked diskOS successfully', 'success');
        } else {
          triggerErrorShake(authInputWrap);
          authError.textContent = data.error || 'Incorrect PIN or password.';
          authError.classList.remove('hidden');
          authInput.select();
          authInput.focus();
        }
      } catch (err) {
        triggerErrorShake(authInputWrap);
        authError.textContent = 'Connection error. Try again.';
        authError.classList.remove('hidden');
      } finally {
        if (submitBtn) {
          submitBtn.disabled = false;
          submitBtn.textContent = origText;
        }
      }
    });
  }

  if (btnLogout) {
    btnLogout.addEventListener('click', () => {
      showConfirmModal({
        title: 'Lock diskOS Web UI',
        desc: 'Log out and require the PIN or password on next access?',
        okText: 'Lock Interface',
        isDanger: false,
        onConfirm: handleUnauthorized
      });
    });
  }

  // -------------------------------------------------------------
  // Sliding Tabs Pill (16-tabs-sliding)
  // -------------------------------------------------------------
  function updateTabsSlider(activeTab, animated = true) {
    if (!tabsSlider || !activeTab) return;
    if (!animated) {
      tabsSlider.style.transition = 'none';
    }
    tabsSlider.style.transform = `translateX(${activeTab.offsetLeft}px)`;
    tabsSlider.style.width = `${activeTab.offsetWidth}px`;
    if (!animated) {
      void tabsSlider.offsetWidth; // force reflow
      tabsSlider.style.transition = '';
    }
  }

  navTabs.forEach(tab => {
    tab.addEventListener('click', () => {
      navTabs.forEach(t => {
        t.classList.remove('active');
        t.setAttribute('aria-selected', 'false');
      });
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));

      tab.classList.add('active');
      tab.setAttribute('aria-selected', 'true');
      updateTabsSlider(tab, true);

      const targetPane = document.getElementById(tab.dataset.tab);
      if (targetPane) targetPane.classList.add('active');

      if (tab.dataset.tab === 'tab-queue') loadQueue();
      if (tab.dataset.tab === 'tab-files') loadFiles(curPath);
      if (tab.dataset.tab === 'tab-settings') loadSettings();
      if (tab.dataset.tab === 'tab-system') loadSystem();
    });
  });

  window.addEventListener('resize', () => {
    const activeTab = document.querySelector('.nav-tab.active');
    if (activeTab) updateTabsSlider(activeTab, false);
  });

  // -------------------------------------------------------------
  // 3D Card Hover Tilt for Turntable (19-card-tilt)
  // -------------------------------------------------------------
  if (vinylTiltWrapper) {
    vinylTiltWrapper.addEventListener('pointermove', (e) => {
      if (e.pointerType === 'touch') return; // ignore touch drags to avoid interfering with scrolling
      const rect = vinylTiltWrapper.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const y = e.clientY - rect.top;
      const midX = rect.width / 2;
      const midY = rect.height / 2;

      // ±10 degrees max rotation
      const rxDeg = ((y - midY) / midY) * -10;
      const ryDeg = ((x - midX) / midX) * 10;
      const gxPct = (x / rect.width) * 100;
      const gyPct = (y / rect.height) * 100;

      vinylTiltWrapper.style.setProperty('--tilt-rx', rxDeg.toFixed(2) + 'deg');
      vinylTiltWrapper.style.setProperty('--tilt-ry', ryDeg.toFixed(2) + 'deg');
      vinylTiltWrapper.style.setProperty('--tilt-gx', gxPct.toFixed(1) + '%');
      vinylTiltWrapper.style.setProperty('--tilt-gy', gyPct.toFixed(1) + '%');

      vinylTiltWrapper.classList.add('is-tilting', 'is-hover');
    });

    vinylTiltWrapper.addEventListener('pointerleave', () => {
      vinylTiltWrapper.classList.remove('is-tilting', 'is-hover');
      vinylTiltWrapper.style.setProperty('--tilt-rx', '0deg');
      vinylTiltWrapper.style.setProperty('--tilt-ry', '0deg');
    });
  }

  // -------------------------------------------------------------
  // Helpers
  // -------------------------------------------------------------
  function fmtTime(ms) {
    if (!ms || ms < 0) return '0:00';
    const s = Math.floor(ms / 1000);
    const m = Math.floor(s / 60);
    const rem = s % 60;
    return m + ':' + (rem < 10 ? '0' : '') + rem;
  }

  function formatBytes(b) {
    if (b < 1024) return b + ' B';
    if (b < 1024 * 1024) return (b / 1024).toFixed(1) + ' KB';
    if (b < 1024 * 1024 * 1024) return (b / (1024 * 1024)).toFixed(1) + ' MB';
    return (b / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  // -------------------------------------------------------------
  // Player Commands & State
  // -------------------------------------------------------------
  async function sendPlayerCmd(action, extra = {}) {
    const payload = Object.assign({ action }, extra);
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(payload));
    } else {
      try {
        await apiFetch('/api/player/cmd', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload)
        });
        setTimeout(fetchPlayerStatus, 150);
      } catch (e) {}
    }
  }

  async function fetchPlayerStatus() {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    try {
      const resp = await apiFetch('/api/player/status');
      const st = await resp.json();
      updatePlayerUI(st);
    } catch (e) {}
  }

  function updatePlayerUI(st) {
    trackTitle.textContent = st.title || 'Ready for Playback';
    trackArtist.textContent = st.artist || 'Select a track to start';
    trackAlbum.textContent = st.album || '--';

    badgeFormat.textContent = st.is_dsd ? 'DSD' : (st.format || 'PCM');
    badgeRate.textContent = st.sample_rate ? (st.sample_rate / 1000) + ' kHz' : '-- kHz';
    badgeBits.textContent = st.encoding_rate ? st.encoding_rate + ' bit' : '-- bit';

    curDurationMs = st.duration_ms || 0;
    timeDur.textContent = fmtTime(curDurationMs);

    if (!isSeeking) {
      const pos = st.position_ms || 0;
      timePos.textContent = fmtTime(pos);
      if (curDurationMs > 0) {
        seekBar.value = Math.min(1000, Math.floor((pos / curDurationMs) * 1000));
      } else {
        seekBar.value = 0;
      }
    }

    if (!isVolChanging && st.volume !== undefined) {
      volBar.value = st.volume;
      volVal.textContent = st.volume;
      if (st.volume > 0) previousVol = st.volume;
      const isMuted = st.volume === 0;
      iconVol.classList.toggle('hidden', isMuted);
      iconVolMute.classList.toggle('hidden', !isMuted);
    }

    // Play/Pause State
    const isPlaying = (st.playing === true || st.state === 2 || st.state === 1);
    if (isPlaying) {
      artDisc.classList.add('playing');
      iconPlay.classList.add('hidden');
      iconPause.classList.remove('hidden');
    } else {
      artDisc.classList.remove('playing');
      iconPlay.classList.remove('hidden');
      iconPause.classList.add('hidden');
    }

    // Battery status
    if (st.battery !== undefined && st.battery >= 0) {
      headerBattery.textContent = st.battery + '%';
    }

    // Play Mode Updates (0 = Loop All, 1 = Repeat 1, 2 = Shuffle, 3 = Sequential)
    if (st.play_mode !== undefined) {
      currentPlayMode = st.play_mode;
      updatePlayModeUI(st.play_mode);
    }

    // Album Artwork
    const trackId = st.path || (st.title ? (st.title + ' - ' + st.artist) : '');
    if (trackId && trackId !== trackArt.dataset.trackId) {
      trackArt.dataset.trackId = trackId;
      trackArt.src = '/api/player/art?t=' + Date.now();
    }

    currentTrackPath = st.path || '';
    updateFileListPlayingState();
    updateQueueListPlayingState();
  }

  function updatePlayModeUI(mode) {
    if (btnShuffle) {
      btnShuffle.classList.toggle('active', mode === 2);
    }
    if (btnRepeat) {
      btnRepeat.classList.toggle('active', mode === 0 || mode === 1);
      btnRepeat.title = mode === 1 ? 'Repeat Current Track' : (mode === 0 ? 'Loop All Tracks' : 'Sequential Mode');
    }
    modePills.forEach(pill => {
      pill.classList.toggle('active', parseInt(pill.dataset.mode, 10) === mode);
    });
  }

  // -------------------------------------------------------------
  // WebSocket Connection
  // -------------------------------------------------------------
  function connectWebSocket() {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    if (wsReconnectTimer) {
      clearTimeout(wsReconnectTimer);
      wsReconnectTimer = null;
    }

    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${proto}//${location.host}/ws?token=${encodeURIComponent(authToken)}`;

    try {
      ws = new WebSocket(wsUrl);

      ws.onopen = () => {
        connStatus.classList.add('online');
        connStatus.querySelector('.indicator-text').textContent = 'Online';
        if (pollTimer) {
          clearInterval(pollTimer);
          pollTimer = null;
        }
      };

      ws.onmessage = (event) => {
        try {
          const st = JSON.parse(event.data);
          updatePlayerUI(st);
        } catch (err) {}
      };

      ws.onclose = () => {
        ws = null;
        if (!authToken || !authModal.classList.contains('hidden')) {
          disconnectWebSocket();
          return;
        }
        connStatus.classList.remove('online');
        connStatus.querySelector('.indicator-text').textContent = 'Connecting...';
        if (!pollTimer) startPolling();
        if (!wsReconnectTimer) {
          wsReconnectTimer = setTimeout(connectWebSocket, 2000);
        }
      };

      ws.onerror = () => {};
    } catch (e) {
      if (!pollTimer) startPolling();
      if (!wsReconnectTimer) {
        wsReconnectTimer = setTimeout(connectWebSocket, 3000);
      }
    }
  }

  function disconnectWebSocket() {
    if (wsReconnectTimer) {
      clearTimeout(wsReconnectTimer);
      wsReconnectTimer = null;
    }
    if (ws) {
      try { ws.close(); } catch (_) {}
      ws = null;
    }
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
  }

  function startPolling() {
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
    if (!authToken || !authModal.classList.contains('hidden')) return;
    fetchPlayerStatus();
    pollTimer = setInterval(fetchPlayerStatus, 1000);
  }

  // -------------------------------------------------------------
  // Player Controls Event Wiring
  // -------------------------------------------------------------
  btnPlay.addEventListener('click', () => sendPlayerCmd('toggle'));
  btnPrev.addEventListener('click', () => sendPlayerCmd('prev'));
  btnNext.addEventListener('click', () => sendPlayerCmd('next'));

  btnShuffle.addEventListener('click', () => {
    const nextMode = (currentPlayMode === 2) ? 0 : 2;
    sendPlayerCmd('play_mode', { mode: nextMode });
    updatePlayModeUI(nextMode);
    showToast(nextMode === 2 ? 'Shuffle enabled' : 'Shuffle disabled');
  });

  btnRepeat.addEventListener('click', () => {
    let nextMode = 0;
    if (currentPlayMode === 0) nextMode = 1;      // Loop All -> Repeat 1
    else if (currentPlayMode === 1) nextMode = 3; // Repeat 1 -> Sequential
    else nextMode = 0;                            // Sequential/Shuffle -> Loop All
    sendPlayerCmd('play_mode', { mode: nextMode });
    updatePlayModeUI(nextMode);
    const names = ['Loop All Tracks', 'Repeat Single Track', 'Shuffle', 'Sequential'];
    showToast(`Playback mode: ${names[nextMode] || 'Default'}`);
  });

  // Volume Bar & Mute
  volBar.addEventListener('input', () => {
    isVolChanging = true;
    volVal.textContent = volBar.value;
    const isMuted = parseInt(volBar.value, 10) === 0;
    iconVol.classList.toggle('hidden', isMuted);
    iconVolMute.classList.toggle('hidden', !isMuted);
  });

  volBar.addEventListener('change', () => {
    const v = parseInt(volBar.value, 10);
    sendPlayerCmd('volume', { val: v });
    if (v > 0) previousVol = v;
    isVolChanging = false;
  });

  btnMute.addEventListener('click', () => {
    const curV = parseInt(volBar.value, 10);
    if (curV > 0) {
      previousVol = curV;
      sendPlayerCmd('volume', { val: 0 });
      volBar.value = 0;
      volVal.textContent = '0';
      iconVol.classList.add('hidden');
      iconVolMute.classList.remove('hidden');
      showToast('Audio muted');
    } else {
      const restoreV = previousVol > 0 ? previousVol : 40;
      sendPlayerCmd('volume', { val: restoreV });
      volBar.value = restoreV;
      volVal.textContent = restoreV;
      iconVol.classList.remove('hidden');
      iconVolMute.classList.add('hidden');
      showToast(`Volume unmuted (${restoreV})`);
    }
  });

  // Seek Bar
  seekBar.addEventListener('input', () => {
    isSeeking = true;
    if (curDurationMs > 0) {
      const ms = Math.floor((seekBar.value / 1000) * curDurationMs);
      timePos.textContent = fmtTime(ms);
    }
  });

  seekBar.addEventListener('change', () => {
    if (curDurationMs > 0) {
      const ms = Math.floor((seekBar.value / 1000) * curDurationMs);
      sendPlayerCmd('seek', { ms });
    }
    isSeeking = false;
  });

  // -------------------------------------------------------------
  // Queue Tab Controller
  // -------------------------------------------------------------
  async function loadQueue() {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    queueList.innerHTML = '<div class="loading-spinner">Loading queue from device database...</div>';

    try {
      const resp = await apiFetch('/api/player/queue');
      queueData = await resp.json();
      renderQueueList();
    } catch (e) {
      queueList.innerHTML = '<div class="sub-hint" style="text-align:center; padding: 20px;">Could not load playback queue.</div>';
    }
  }

  function renderQueueList() {
    queueList.innerHTML = '';
    const qFilter = (queueSearch.value || '').trim().toLowerCase();
    const filtered = queueData.filter(item => {
      if (!qFilter) return true;
      return (item.title && item.title.toLowerCase().includes(qFilter)) ||
             (item.artist && item.artist.toLowerCase().includes(qFilter)) ||
             (item.album && item.album.toLowerCase().includes(qFilter)) ||
             (item.path && item.path.toLowerCase().includes(qFilter));
    });

    queueCount.textContent = `${filtered.length} track${filtered.length === 1 ? '' : 's'}`;

    if (!filtered || filtered.length === 0) {
      queueList.innerHTML = `<div class="sub-hint" style="text-align:center; padding: 24px;">${qFilter ? 'No matching tracks found in queue.' : 'Queue is empty. Play a file or playlist to populate queue.'}</div>`;
      return;
    }

    filtered.forEach((item) => {
      const row = document.createElement('div');
      row.className = 'queue-row';
      row.dataset.path = item.path;
      row.dataset.idx = item.idx;

      const isCurrent = currentTrackPath && (currentTrackPath === item.path || currentTrackPath.endsWith('/' + item.path));
      if (isCurrent) row.classList.add('is-playing');

      row.innerHTML = `
        <div class="queue-left">
          <div class="queue-idx">${item.idx + 1}</div>
          <div class="eq-bars" aria-hidden="true">
            <span class="eq-bar"></span>
            <span class="eq-bar"></span>
            <span class="eq-bar"></span>
          </div>
          <div class="queue-info">
            <span class="queue-title">${item.title || 'Unknown Track'}</span>
            <span class="queue-sub">${item.artist || 'Unknown Artist'}${item.album ? ' • ' + item.album : ''}</span>
          </div>
        </div>
        <div class="queue-dur">${fmtTime(item.duration)}</div>
      `;

      row.addEventListener('click', () => {
        sendPlayerCmd('play_idx', { idx: item.idx });
        showToast(`Playing: ${item.title || 'Track ' + (item.idx + 1)}`, 'info');
      });

      queueList.appendChild(row);
    });
  }

  function updateQueueListPlayingState() {
    document.querySelectorAll('.queue-row[data-path]').forEach(row => {
      const p = row.dataset.path;
      const isCur = currentTrackPath && (currentTrackPath === p || currentTrackPath.endsWith('/' + p));
      row.classList.toggle('is-playing', isCur);
    });
  }

  queueSearch.addEventListener('input', () => {
    queueSearchClear.classList.toggle('hidden', !queueSearch.value);
    renderQueueList();
  });

  queueSearchClear.addEventListener('click', () => {
    queueSearch.value = '';
    queueSearchClear.classList.add('hidden');
    renderQueueList();
    queueSearch.focus();
  });

  btnRefreshQueue.addEventListener('click', () => {
    loadQueue();
    showToast('Queue refreshed');
  });

  btnClearQueue.addEventListener('click', () => {
    showConfirmModal({
      title: 'Clear Playback Queue',
      desc: 'Are you sure you want to remove all songs from the active queue?',
      okText: 'Clear Queue',
      isDanger: true,
      onConfirm: async () => {
        try {
          await sendPlayerCmd('clear_queue');
          showToast('Queue cleared', 'success');
          loadQueue();
        } catch (e) {
          showToast('Failed to clear queue', 'danger');
        }
      }
    });
  });

  // -------------------------------------------------------------
  // Files Controller
  // -------------------------------------------------------------
  async function loadFiles(path) {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    curPath = path;
    updateBreadcrumbs(path);
    fileList.innerHTML = '<div class="loading-spinner">Loading directory...</div>';

    try {
      const resp = await apiFetch('/api/files/list?path=' + encodeURIComponent(path));
      currentDirectoryFiles = await resp.json();
      renderFileList();
    } catch (e) {
      if (authToken) {
        fileList.innerHTML = '<div class="sub-hint" style="text-align:center; padding: 20px;">Could not load files.</div>';
      }
    }
  }

  function updateBreadcrumbs(path) {
    filesBreadcrumb.innerHTML = '';
    const rootSpan = document.createElement('span');
    rootSpan.className = 'crumb-root';
    rootSpan.textContent = 'SD Card';
    rootSpan.addEventListener('click', () => loadFiles(''));
    filesBreadcrumb.appendChild(rootSpan);

    if (!path) return;
    const parts = path.split('/').filter(Boolean);
    let accum = '';
    for (let i = 0; i < parts.length; i++) {
      const sep = document.createTextNode(' / ');
      filesBreadcrumb.appendChild(sep);
      accum += (accum ? '/' : '') + parts[i];
      const p = accum;
      const span = document.createElement('span');
      span.className = 'crumb-item';
      span.textContent = parts[i];
      span.addEventListener('click', () => loadFiles(p));
      filesBreadcrumb.appendChild(span);
    }
  }

  function renderFileList() {
    fileList.innerHTML = '';
    const filter = (fileSearch.value || '').trim().toLowerCase();
    const items = currentDirectoryFiles.filter(item => {
      if (!filter) return true;
      return item.name && item.name.toLowerCase().includes(filter);
    });

    if (!items || items.length === 0) {
      fileList.innerHTML = `<div class="sub-hint" style="text-align:center; padding: 20px;">${filter ? 'No matching files found.' : 'Empty folder'}</div>`;
      return;
    }

    // Sort: directories first, then alphabetical
    items.sort((a, b) => {
      if (a.is_dir && !b.is_dir) return -1;
      if (!a.is_dir && b.is_dir) return 1;
      return a.name.localeCompare(b.name);
    });

    items.forEach(item => {
      const row = document.createElement('div');
      row.className = 'file-row';
      const itemRelPath = curPath ? curPath + '/' + item.name : item.name;
      const fullPath = '/tmp/sdcard/' + itemRelPath;
      row.dataset.path = fullPath;

      if (currentTrackPath && (currentTrackPath === fullPath || currentTrackPath.endsWith('/' + itemRelPath))) {
        row.classList.add('is-playing');
      }

      const left = document.createElement('div');
      left.className = 'file-left';

      const icon = document.createElement('div');
      icon.className = 'file-icon ' + (item.is_dir ? 'folder' : 'audio');
      icon.innerHTML = item.is_dir
        ? '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path d="M10 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z"/></svg>'
        : '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor"><path d="M12 3v10.55c-.59-.34-1.27-.55-2-.55-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4V7h4V3h-6z"/></svg>';

      const name = document.createElement('div');
      name.className = 'file-name';
      name.textContent = item.name;
      if (!item.is_dir && item.size) {
        const sz = document.createElement('span');
        sz.style.color = 'var(--text-muted)';
        sz.style.fontSize = '11px';
        sz.style.marginLeft = '6px';
        sz.textContent = '(' + formatBytes(item.size) + ')';
        name.appendChild(sz);
      }

      left.appendChild(icon);
      left.appendChild(name);

      left.addEventListener('click', () => {
        if (item.is_dir) {
          const next = curPath ? curPath + '/' + item.name : item.name;
          loadFiles(next);
        } else {
          sendPlayerCmd('play_file', { path: fullPath });
          showToast(`Playing ${item.name}`, 'info');
        }
      });

      const actions = document.createElement('div');
      actions.className = 'file-actions';

      if (!item.is_dir) {
        // Play action
        const playBtn = document.createElement('button');
        playBtn.className = 'action-btn play';
        playBtn.title = 'Play';
        playBtn.setAttribute('aria-label', `Play ${item.name}`);
        playBtn.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>';
        playBtn.addEventListener('click', (e) => {
          e.stopPropagation();
          sendPlayerCmd('play_file', { path: fullPath });
          showToast(`Playing ${item.name}`, 'info');
        });
        actions.appendChild(playBtn);

        // Download action
        const dlBtn = document.createElement('a');
        dlBtn.className = 'action-btn';
        dlBtn.title = 'Download';
        dlBtn.setAttribute('aria-label', `Download ${item.name}`);
        dlBtn.href = '/api/files/download?path=' + encodeURIComponent(itemRelPath) + '&token=' + encodeURIComponent(authToken);
        dlBtn.download = item.name;
        dlBtn.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M19.35 10.04C18.67 6.59 15.64 4 12 4 9.11 4 6.6 5.64 5.35 8.04 2.34 8.36 0 10.91 0 14c0 3.31 2.69 6 6 6h13c2.76 0 5-2.24 5-5 0-2.64-2.05-4.78-4.65-4.96zM17 13l-5 5-5-5h3V9h4v4h3z"/></svg>';
        actions.appendChild(dlBtn);
      }

      // Delete action
      const delBtn = document.createElement('button');
      delBtn.className = 'action-btn del';
      delBtn.title = 'Delete';
      delBtn.setAttribute('aria-label', `Delete ${item.name}`);
      delBtn.innerHTML = '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>';
      delBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        showConfirmModal({
          title: `Delete ${item.is_dir ? 'Folder' : 'File'}`,
          desc: `Permanently delete "${item.name}" from the MicroSD card?`,
          okText: 'Delete Permanently',
          isDanger: true,
          onConfirm: async () => {
            const itemPath = curPath ? curPath + '/' + item.name : item.name;
            try {
              await apiFetch('/api/files/delete?path=' + encodeURIComponent(itemPath), { method: 'DELETE' });
              showToast(`Deleted ${item.name}`, 'success');
              loadFiles(curPath);
            } catch (err) {
              showToast('Could not delete item', 'danger');
            }
          }
        });
      });
      actions.appendChild(delBtn);

      row.appendChild(left);
      row.appendChild(actions);
      fileList.appendChild(row);
    });
  }

  function updateFileListPlayingState() {
    document.querySelectorAll('.file-row[data-path]').forEach(row => {
      const p = row.dataset.path;
      const isCur = currentTrackPath && (currentTrackPath === p || currentTrackPath.endsWith('/' + p) || ('/tmp/sdcard/' + p) === currentTrackPath);
      row.classList.toggle('is-playing', isCur);
    });
  }

  fileSearch.addEventListener('input', () => {
    fileSearchClear.classList.toggle('hidden', !fileSearch.value);
    renderFileList();
  });

  fileSearchClear.addEventListener('click', () => {
    fileSearch.value = '';
    fileSearchClear.classList.add('hidden');
    renderFileList();
    fileSearch.focus();
  });

  btnRefreshFiles.addEventListener('click', () => {
    loadFiles(curPath);
    showToast('Directory refreshed');
  });

  if (btnNewFolder) {
    btnNewFolder.addEventListener('click', () => {
      newFolderDest.textContent = curPath ? '/' + curPath : 'SD Card Root';
      newFolderInput.value = '';
      openModal(newFolderModal);
      setTimeout(() => newFolderInput.focus(), 100);
    });
  }

  if (newFolderCancel) {
    newFolderCancel.addEventListener('click', () => {
      closeModal(newFolderModal);
    });
  }

  if (newFolderForm) {
    newFolderForm.addEventListener('submit', async (e) => {
      e.preventDefault();
      const folderName = newFolderInput.value.trim();
      if (!folderName || folderName.includes('/') || folderName.includes('\\') || folderName === '.' || folderName === '..') {
        triggerErrorShake(newFolderInputWrap);
        showToast('Please enter a valid folder name (no slashes)', 'warning');
        newFolderInput.focus();
        return;
      }

      const submitBtn = document.getElementById('new-folder-submit');
      const origText = submitBtn ? submitBtn.textContent : 'Create Folder';
      if (submitBtn) {
        submitBtn.disabled = true;
        submitBtn.textContent = 'Creating...';
      }

      try {
        const resp = await apiFetch('/api/files/mkdir', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ dir: curPath, name: folderName })
        });
        const data = await resp.json();
        if (data.ok) {
          closeModal(newFolderModal);
          showToast(`Folder "${folderName}" created successfully`, 'success');
          loadFiles(curPath);
        } else {
          triggerErrorShake(newFolderInputWrap);
          showToast(data.error || 'Failed to create folder', 'danger');
        }
      } catch (err) {
        triggerErrorShake(newFolderInputWrap);
        showToast('Could not create folder. Check connection.', 'danger');
      } finally {
        if (submitBtn) {
          submitBtn.disabled = false;
          submitBtn.textContent = origText;
        }
      }
    });
  }

  // Upload Management
  browseTrigger.addEventListener('click', () => fileInput.click());

  dropZone.addEventListener('dragover', (e) => {
    e.preventDefault();
    dropZone.classList.add('dragover');
  });

  dropZone.addEventListener('dragleave', () => {
    dropZone.classList.remove('dragover');
  });

  dropZone.addEventListener('drop', (e) => {
    e.preventDefault();
    dropZone.classList.remove('dragover');
    if (e.dataTransfer.files && e.dataTransfer.files.length > 0) {
      uploadFiles(Array.from(e.dataTransfer.files));
    }
  });

  fileInput.addEventListener('change', () => {
    if (fileInput.files && fileInput.files.length > 0) {
      uploadFiles(Array.from(fileInput.files));
      fileInput.value = '';
    }
  });

  async function uploadFiles(files) {
    if (!files.length) return;
    uploadStatus.classList.remove('hidden');

    for (let i = 0; i < files.length; i++) {
      const file = files[i];
      uploadFilename.textContent = `Uploading [${i+1}/${files.length}]: ${file.name}`;
      uploadPct.textContent = '0%';
      uploadFill.style.width = '0%';

      await new Promise((resolve, reject) => {
        const xhr = new XMLHttpRequest();
        const uploadUrl = '/api/files/upload?dir=' + encodeURIComponent(curPath) + '&filename=' + encodeURIComponent(file.name);
        xhr.open('POST', uploadUrl, true);
        if (authToken) xhr.setRequestHeader('Authorization', 'Bearer ' + authToken);

        xhr.upload.onprogress = (ev) => {
          if (ev.lengthComputable) {
            const pct = Math.floor((ev.loaded / ev.total) * 100);
            uploadPct.textContent = pct + '%';
            uploadFill.style.width = pct + '%';
          }
        };

        xhr.onload = () => {
          if (xhr.status >= 200 && xhr.status < 300) resolve();
          else reject(new Error('Upload failed'));
        };

        xhr.onerror = () => reject(new Error('Network error during upload'));
        xhr.send(file);
      }).catch(err => {
        showToast(`Upload failed for ${file.name}`, 'danger');
      });
    }

    uploadFilename.textContent = 'Upload complete!';
    showToast('All files uploaded to MicroSD', 'success');
    setTimeout(() => {
      uploadStatus.classList.add('hidden');
      loadFiles(curPath);
    }, 1000);
  }

  // -------------------------------------------------------------
  // Settings Tab Controller
  // -------------------------------------------------------------
  async function loadSettings() {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    try {
      const resp = await apiFetch('/api/settings');
      const s = await resp.json();

      if (s.play_mode !== undefined) {
        updatePlayModeUI(s.play_mode);
      }
      if (s.sleep_timer !== undefined && settingSleep) {
        settingSleep.value = s.sleep_timer.toString();
      }
      if (s.brightness !== undefined && settingBrightness) {
        settingBrightness.value = s.brightness;
        brightnessVal.textContent = s.brightness;
      }
    } catch (e) {}
  }

  modePills.forEach(pill => {
    pill.addEventListener('click', async () => {
      const mode = parseInt(pill.dataset.mode, 10);
      try {
        await apiFetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ play_mode: mode })
        });
        updatePlayModeUI(mode);
        showToast(`Default playback mode set to ${pill.textContent}`, 'success');
      } catch (e) {
        showToast('Failed to update playback mode', 'danger');
      }
    });
  });

  if (settingSleep) {
    settingSleep.addEventListener('change', async () => {
      const val = parseInt(settingSleep.value, 10);
      try {
        await apiFetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ sleep_timer: val })
        });
        const label = settingSleep.options[settingSleep.selectedIndex].text;
        showToast(`Sleep timer set to ${label}`, 'success');
      } catch (e) {
        showToast('Failed to update sleep timer', 'danger');
      }
    });
  }

  if (settingBrightness) {
    settingBrightness.addEventListener('input', () => {
      brightnessVal.textContent = settingBrightness.value;
    });

    settingBrightness.addEventListener('change', async () => {
      const val = parseInt(settingBrightness.value, 10);
      try {
        await apiFetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ brightness: val })
        });
        showToast(`Backlight level: ${val} / 40`, 'info');
      } catch (e) {
        showToast('Failed to set brightness', 'danger');
      }
    });
  }

  if (formChangePin) {
    formChangePin.addEventListener('submit', async (e) => {
      e.preventDefault();
      const newPin = settingNewPin.value.trim();
      if (!newPin) {
        showToast('PIN cannot be empty', 'warning');
        return;
      }
      try {
        const resp = await apiFetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ pin: newPin })
        });
        const data = await resp.json();
        if (data.ok) {
          settingNewPin.value = '';
          showToast('PIN updated and persisted in /usr/data/diskos.conf', 'success');
        } else {
          showToast(data.error || 'Failed to update PIN', 'danger');
        }
      } catch (e) {
        showToast('Error updating PIN', 'danger');
      }
    });
  }

  if (btnRestartUi) {
    btnRestartUi.addEventListener('click', () => {
      showConfirmModal({
        title: 'Restart diskOS UI',
        desc: 'Restart mq_ui on the Snowsky Disc? Active playback will continue smoothly.',
        okText: 'Restart UI',
        isDanger: false,
        onConfirm: async () => {
          try {
            await apiFetch('/api/settings', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ restart_ui: true })
            });
            showToast('diskOS UI restarted', 'success');
          } catch (e) {
            showToast('Failed to restart UI', 'danger');
          }
        }
      });
    });
  }

  if (btnRebootDev) {
    btnRebootDev.addEventListener('click', () => {
      showConfirmModal({
        title: 'Reboot Snowsky Disc',
        desc: 'Reboot the device hardware? Connection will temporarily disconnect.',
        okText: 'Reboot Now',
        isDanger: true,
        onConfirm: async () => {
          try {
            await apiFetch('/api/settings', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ reboot: true })
            });
            showToast('Reboot signal sent to Snowsky Disc', 'warning');
          } catch (e) {
            showToast('Failed to trigger reboot', 'danger');
          }
        }
      });
    });
  }

  // -------------------------------------------------------------
  // System Tab Controller
  // -------------------------------------------------------------
  async function loadSystem() {
    if (!authToken || !authModal.classList.contains('hidden')) return;
    try {
      const resp = await apiFetch('/api/system/info');
      const sys = await resp.json();

      sysBatPct.textContent = sys.battery !== undefined ? sys.battery + '%' : '--';
      sysBatVolt.textContent = sys.voltage_now ? Math.floor(sys.voltage_now / 1000) + ' mV' : '--';
      sysCharging.textContent = sys.charging ? 'Charging (Power Connected)' : 'Discharging (Battery)';

      if (sys.sd_total && sys.sd_free) {
        const freeGB = (sys.sd_free / (1024 * 1024 * 1024)).toFixed(1);
        const totalGB = (sys.sd_total / (1024 * 1024 * 1024)).toFixed(1);
        sysSdFree.textContent = freeGB + ' GB';
        sysSdTotal.textContent = totalGB + ' GB';
        const usedPct = Math.floor(((sys.sd_total - sys.sd_free) / sys.sd_total) * 100);
        sysStorageFill.style.width = usedPct + '%';
      }
    } catch (e) {}
  }

  if (btnRescanLib) {
    btnRescanLib.addEventListener('click', async () => {
      btnRescanLib.disabled = true;
      btnRescanLib.textContent = 'Scanning library...';
      try {
        await apiFetch('/api/library/rescan', { method: 'POST' });
        showToast('Library rescan initiated on Snowsky Disc', 'success');
      } catch (e) {
        showToast('Failed to trigger scan', 'danger');
      } finally {
        setTimeout(() => {
          btnRescanLib.disabled = false;
          btnRescanLib.textContent = 'Trigger Library Rescan';
        }, 3000);
      }
    });
  }

  // -------------------------------------------------------------
  // Initialization
  // -------------------------------------------------------------
  // Position initial tab slider pill without animation
  const initialActiveTab = document.querySelector('.nav-tab.active');
  if (initialActiveTab) {
    updateTabsSlider(initialActiveTab, false);
  }

  if (!authToken) {
    showAuthModal();
  } else {
    hideAuthModal();
    connectWebSocket();
    loadFiles('');
    loadQueue();
    loadSettings();
    loadSystem();
  }

})();
