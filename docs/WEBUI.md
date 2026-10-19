# diskOS Web UI & Remote Control Server

The **diskOS Web UI** is an embedded web interface and HTTP/WebSocket server that runs directly on the FiiO Snowsky Disc (Ingenic X2000 SoC). It provides complete browser-based remote control of playback, volume, queue management, file browsing, track selection, folder creation, and direct audio file uploads over local Wi-Fi.

---

## Architecture Overview

```
                      +------------------------------------------+
                      |         Web Browser / Mobile Device       |
                      |   (HTML5 / CSS Transitions / ES6 JS)     |
                      +--------------------+---------------------+
                                           | HTTP (port 8080)
                                           | WebSocket (/ws)
+------------------------------------------v------------------------------------------+
|  FiiO Snowsky Disc (Linux 4.4.94 / Ingenic X2000)                                   |
|                                                                                      |
|  +--------------------------------------------------------------------------------+  |
|  |  diskos-webd  (Static MIPS32r2 Binary, ~149 KB)                                |  |
|  |  - Embedded POSIX multi-threaded HTTP 1.1 daemon                               |  |
|  |  - SHA-256 session token authentication                                        |  |
|  |  - Path-traversal jailing (restricted strictly to /tmp/sdcard)                 |  |
|  |  - Static asset streaming with caching headers                                |  |
|  |  - Multi-client broadcast WebSocket status engine                              |  |
|  +-------------------------+----------------------------------+-------------------+  |
|                            | POSIX Message Queue (/player)    | Shared State JSON    |
|                            v                                  v                      |
|  +-----------------------------------+              +-----------------------------+  |
|  |  FiiO Audio Daemon (mq_player)    |              |  diskOS UI (mq_ui)          |  |
|  |  - Audio decoder & hardware DACs  |              |  - LVGL 9 display & screens |  |
|  |  - POSIX /player message queue    |              |  - Apps > Web Server        |  |
|  +-----------------------------------+              |  - Writes /tmp/diskos_player|  |
|                                                     +-----------------------------+  |
+--------------------------------------------------------------------------------------+
```

### Components

1. **`diskos-webd` (Daemon Binary)**:
   - Written in C99, statically compiled for `mipsel-linux-musl` (`-static -pthread`).
   - Zero external dynamic dependencies; small memory footprint (~2.5 MB RSS).
   - Serves static assets (`index.html`, `style.css`, `app.js`) and REST API endpoints.
   - Real-time WebSocket `/ws` feeds push status updates at up to 10 Hz with debounce.
   - Strictly jailed to `/tmp/sdcard` for file operations and uploads to protect system partitions.

2. **Web Frontend (`web/`)**:
   - Single-Page Application (SPA) built with vanilla semantic HTML5, CSS custom properties, and modern ES6 JavaScript.
   - Fluid transitions and feedback adhering to the `transitions-dev` and `better-interface` design systems.
   - Responsive layout adapting seamlessly from handheld mobile screens (320px–480px) to desktop browsers.
   - Custom styled scrollbars matching active theme tokens (Disc Teal, Amber, Ruby, Violet, Emerald, Monochrome).
   - Full keyboard accessibility, touch gestures, and optimistic UI updates.

3. **On-Device Controller (`ui/webui.c`)**:
   - Integrated into diskOS Apps screen: **Apps > Web Server**.
   - Live server state monitoring (`RUNNING` / `STOPPED` / `SD READY` / `NOT INSTALLED`).
   - One-tap start/stop/install button.
   - Dynamic IP display (`http://<ip>:8080/`).
   - PIN / Password customization directly on-device using diskOS virtual keyboard.
   - Synchronizes playback metadata into `/tmp/diskos_player.json` every 500ms.

---

## Deployment Methods

diskOS supports three deployment paths depending on user preference and workflow:

### Path A: Firmware Installer Embedding (Clean Flashes)

For users building or flashing a custom diskOS firmware image using `diskos_installer`:

1. **How it works**:
   - The installer automatically embeds `diskos-webd`, `diskos-web.sh`, and `diskos-web-assets.tar.gz` into the SquashFS rootfs (`opt/diskos/bin/` and `opt/diskos/web.tar.gz`).
   - On first boot, the fail-closed boot hook [`payload/S97diskos_install`](file:///home/luii/Documents/diskos/payload/S97diskos_install#L502-L516) unpacks and installs the binaries into the persistent UBIFS partition `/usr/data/bin/` and `/usr/data/web/`.
2. **Usage**:
   - Run the standard installer:
     ```sh
     ./diskos-installer gui
     ```
   - Flash the device. Once booted, the Web Server is ready immediately under **Apps > Web Server**.

---

### Path B: MicroSD Card Package (No Re-flashing Required)

For users who already have diskOS installed and want the Web UI without wiping or reflashing the system software:

1. **Generate or Download the SD Package**:
   - In the diskOS source tree, run:
     ```sh
     ./web/package-sdcard.sh
     ```
   - This produces `dist/diskos-web-sdcard.zip` and `dist/diskos-web-sdcard.tar.gz`.
2. **Copy to MicroSD**:
   - Extract the `diskos` folder from the archive to the root of your MicroSD card.
   - Your MicroSD card will have:
     ```
     /MicroSD/diskos/
     ├── diskos-webd
     ├── diskos-web.sh
     ├── diskos-web-assets.tar.gz
     ├── install.sh
     └── README.txt
     ```
3. **Install on Device**:
   - Insert the MicroSD card into the FiiO Snowsky Disc.
   - Turn on the player and navigate to **Apps > Web Server**.
   - The screen will detect the package and display:
     - **Status: SD READY**
     - **Button: Install & Start**
   - Tap **Install & Start**. diskOS will automatically copy the binaries to `/usr/data/bin/`, unpack web assets to `/usr/data/web/`, and start the web server!
   - Alternatively, if you have SSH or serial access, you can run:
     ```sh
     sh /tmp/sdcard/diskos/install.sh
     ```

---

### Path C: Live Developer Deployment (over Wi-Fi)

For developers actively modifying `web/` HTML/CSS/JS or C daemon code:

1. Enable **Debug Mode** under **Settings > System > Debug Mode** on the device to start SSH.
2. Run the deployment script from your workstation:
   ```sh
   DISKOS_IP="<device-ip>" DISKOS_PW="<ssh-password>" ./web/deploy.sh
   ```
   Or pass parameters directly:
   ```sh
   ./web/deploy.sh 10.10.10.145 fvju4zcn4u
   ```
3. The script compiles the static MIPS binary, packs web assets, streams them over SSH, updates `/usr/data/`, and restarts `diskos-webd` without interrupting LVGL.

---

## On-Device Controls (Apps > Web Server)

Open **Apps > Web Server** on the player screen:

| Control | Description |
| :--- | :--- |
| **Status Indicator** | Displays current operational state: `RUNNING` (accent color), `STOPPED` (muted gray), `SD READY` (accent color), or `NOT INSTALLED`. |
| **Access URL** | Displays `http://<device-ip>:8080/` when connected to Wi-Fi. |
| **PIN / Password** | Shows current web authentication PIN/password (default: `1234`). |
| **Change PIN** | Opens the on-screen keyboard to set a custom PIN or password. If the server is running, it restarts automatically with the new password. |
| **Action Button** | Single tap toggles server state: **Start Server**, **Stop Server**, or **Install & Start** when an SD package is detected. |

---

## Configuration

Settings are persisted in `/usr/data/diskos.conf`:

```ini
# diskOS Web UI configuration
web_pw=1234
```

To run the daemon manually with custom options:
```sh
/usr/data/bin/diskos-webd -p <port> -w <web-dir> -c <config-file> -a <password>
```

- `-p`: HTTP listen port (default: `8080`)
- `-w`: Web assets directory (default: `/usr/data/web`)
- `-c`: Configuration file path (default: `/usr/data/diskos.conf`)
- `-a`: Authentication password / PIN (default: `1234`)

To control the server from a shell:
```sh
/usr/data/bin/diskos-web.sh start
/usr/data/bin/diskos-web.sh stop
/usr/data/bin/diskos-web.sh status
/usr/data/bin/diskos-web.sh install
```

---

## REST & WebSocket API Reference

All REST endpoints return JSON. State modifications and data queries require authentication via the `Authorization: Bearer <token>` header or `?token=<token>` query parameter.

### 1. Authentication

#### `POST /api/auth`
Validate user credentials and obtain a session token.
- **Request**:
  ```json
  { "password": "1234" }
  ```
- **Response**:
  ```json
  { "ok": true, "token": "90d9c0b843f31768a04cbc8a247894f6" }
  ```
- Subsequent requests authenticate with header:
  `Authorization: Bearer 90d9c0b843f31768a04cbc8a247894f6`

---

### 2. Player Status & Transport

#### `GET /api/player/status`
Returns real-time playback state, track metadata, duration, position, volume, sample rate, hardware flags, and battery level.
- **Response**:
  ```json
  {
    "title": "Song Title",
    "artist": "Artist Name",
    "album": "Album Name",
    "path": "/tmp/sdcard/Music/Song.flac",
    "duration": 245000,
    "duration_ms": 245000,
    "position": 123400,
    "position_ms": 123400,
    "volume": 42,
    "playing": true,
    "state": 2,
    "sample_rate": 96000,
    "is_dsd": false,
    "battery": 60,
    "play_mode": 1
  }
  ```

#### `POST /api/player/cmd`
Send playback transport, navigation, volume, or track commands.
- **Payload Examples**:
  - `{"cmd": "toggle"}`: Toggle play/pause
  - `{"cmd": "play"}`: Resume playback
  - `{"cmd": "pause"}`: Pause playback
  - `{"cmd": "next"}`: Skip to next track
  - `{"cmd": "prev"}`: Skip to previous track
  - `{"cmd": "vol_up"}`: Increment volume by 1
  - `{"cmd": "vol_down"}`: Decrement volume by 1
  - `{"cmd": "volume", "val": 35}`: Set volume level (0-100)
  - `{"cmd": "seek", "val": 45}`: Seek to position in seconds
  - `{"cmd": "play_path", "path": "/tmp/sdcard/Music/track.flac"}`: Start playback of specific file
  - `{"cmd": "play_index", "val": 4}`: Jump to index in playback queue

---

### 3. Queue Management & Cover Art

#### `GET /api/player/queue`
Retrieve currently loaded playlist queue and active playback index.

#### `GET /api/player/art`
Returns the active track's album cover image (`image/jpeg` or `image/png`) cached on the SD card (`/tmp/sdcard/.diskos/artcache/`).

---

### 4. File Management & Jailing

To protect system partitions (`/usr/data`, `/etc`, SquashFS rootfs), all file browsing and upload APIs enforce a strict chroot-style path jail: **paths MUST resolve within `/tmp/sdcard`**. Requests containing `..` escaping or referencing paths outside `/tmp/sdcard` return `403 Forbidden`.

#### `GET /api/files/list?path=/tmp/sdcard/Music`
List files and subdirectories.
- **Response**:
  ```json
  {
    "current": "/tmp/sdcard/Music",
    "parent": "/tmp/sdcard",
    "entries": [
      { "name": "Album 1", "is_dir": true, "size": 0, "mtime": 1729000000 },
      { "name": "01.flac", "is_dir": false, "size": 32540000, "mtime": 1729000000 }
    ]
  }
  ```

#### `GET /api/files/download?path=/tmp/sdcard/Music/01.flac`
Download an audio file directly from the player.

#### `POST /api/files/mkdir`
Create a new directory on the MicroSD card.
- **Payload**:
  ```json
  { "dir": "/tmp/sdcard/Music/New Folder" }
  ```

#### `DELETE /api/files/delete`
Delete an audio file or empty directory.
- **Payload**:
  ```json
  { "path": "/tmp/sdcard/Music/track.flac" }
  ```

#### `POST /api/files/upload?path=/tmp/sdcard/Music`
Upload an audio file via multipart form data (`audio/*`). The uploaded file is streamed directly into `/tmp/sdcard/` without consuming RAM.

---

### 5. System & Settings

#### `GET /api/system/info`
Retrieve battery percentage, charging status, uptime, system load averages, Wi-Fi IP address, and MicroSD storage usage.

#### `POST /api/library/rescan`
Triggers the device music database scanner.

#### `GET /api/settings` and `POST /api/settings`
Read or update configuration fields (such as `web_pw`).

---

### 6. WebSocket Live Stream

#### `GET /ws`
Upgrades the connection to WebSocket.
- The server pushes a JSON status message immediately upon connection and whenever track, position, or volume state changes.
- Eliminates HTTP polling overhead and provides 60 FPS slider accuracy.

---

## Security Model

1. **Authentication Gate**:
   - Unauthorized requests to `/api/command`, `/api/upload`, `/api/delete`, `/api/mkdir`, and `/api/settings` receive `401 Unauthorized`.
   - The authentication token is a high-entropy hex string stored in memory and verified on every request.
2. **Directory Jailing**:
   - All file operations verify `realpath(path)` starts with `/tmp/sdcard/`.
   - Symbol links pointing outside `/tmp/sdcard` are rejected.
3. **Fail-Closed Integration**:
   - The web server runs completely unprivileged relative to the audio engine.
   - If `diskos-webd` is stopped or terminated, `mq_player` and LVGL `mq_ui` continue running undisturbed.
