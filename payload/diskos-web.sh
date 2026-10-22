#!/bin/sh
# diskOS Web UI Server control script
# Manages diskos-webd on port 8080

DAEMON="/usr/data/bin/diskos-webd"
WEB_DIR="/usr/data/web"
CONF="/usr/data/diskos.conf"
PIDFILE="/tmp/diskos-webd.pid"

is_running() {
    pidof diskos-webd >/dev/null 2>&1
}

find_sd_dir() {
    for d in /tmp/sdcard/diskos /tmp/sdcard/diskos-web /tmp/sdcard/.diskos; do
        if [ -d "$d" ]; then
            echo "$d"
            return 0
        fi
    done
    return 1
}

install_from_sd() {
    SD_DIR=$(find_sd_dir)
    if [ -z "$SD_DIR" ]; then
        return 1
    fi
    mkdir -p /usr/data/bin "$WEB_DIR" 2>/dev/null

    if [ -f "$SD_DIR/diskos-webd" ]; then
        cp -f "$SD_DIR/diskos-webd" "$DAEMON" 2>/dev/null
        chmod 755 "$DAEMON" 2>/dev/null
    fi

    if [ -f "$SD_DIR/diskos-web.sh" ]; then
        cp -f "$SD_DIR/diskos-web.sh" /usr/data/bin/diskos-web.sh 2>/dev/null
        chmod 755 /usr/data/bin/diskos-web.sh 2>/dev/null
    fi

    if [ -f "$SD_DIR/diskos-web-assets.tar.gz" ]; then
        gunzip -c "$SD_DIR/diskos-web-assets.tar.gz" | tar -x -C "$WEB_DIR" 2>/dev/null
    elif [ -f "$SD_DIR/web.tar.gz" ]; then
        gunzip -c "$SD_DIR/web.tar.gz" | tar -x -C "$WEB_DIR" 2>/dev/null
    elif [ -d "$SD_DIR/web" ]; then
        cp -rf "$SD_DIR/web/"* "$WEB_DIR/" 2>/dev/null
    fi
    return 0
}

case "$1" in
    start)
        if is_running; then
            echo "already running"
            exit 0
        fi
        if [ ! -x "$DAEMON" ] || [ ! -f "$WEB_DIR/index.html" ]; then
            install_from_sd
        fi
        [ -x "$DAEMON" ] || { echo "daemon binary missing"; exit 1; }
        mkdir -p "$WEB_DIR" 2>/dev/null
        PW=$(awk -F= '$1=="web_pw"{print $2}' "$CONF" 2>/dev/null)
        [ -n "$PW" ] || PW="1234"
        setsid "$DAEMON" -p 8080 -w "$WEB_DIR" -c "$CONF" -a "$PW" </dev/null >/dev/null 2>&1 &
        sleep 1
        if is_running; then
            echo "started"
        else
            echo "failed to start"
            exit 1
        fi
        ;;
    stop)
        for p in $(pidof diskos-webd 2>/dev/null); do
            kill "$p" 2>/dev/null
        done
        sleep 1
        for p in $(pidof diskos-webd 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
        rm -f "$PIDFILE" /tmp/diskos_ws_active 2>/dev/null
        echo "stopped"
        ;;
    status)
        if is_running; then
            echo "running"
        else
            echo "stopped"
        fi
        ;;
    install)
        if install_from_sd; then
            echo "installed"
        else
            echo "no SD package found"
            exit 1
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|status|install}"
        exit 1
        ;;
esac

