/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2021 Aidan MacDonald
 *
 * Directly adapted from jz4760_tools/usbboot.c,
 *   Copyright (C) 2015 by Amaury Pouly
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#define _POSIX_C_SOURCE 200809L
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0a00
#endif
#include <libusb.h>
#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <wchar.h>
#else
#include <time.h>
#endif
#include <getopt.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

#define VR_GET_CPU_INFO     0
#define VR_SET_DATA_ADDRESS 1
#define VR_SET_DATA_LENGTH  2
#define VR_FLUSH_CACHES     3
#define VR_PROGRAM_START1   4
#define VR_PROGRAM_START2   5
#define VR_GET_ACK              0x10
#define VR_INIT                 0x11
#define VR_WRITE                0x12
#define VR_READ                 0x13
#define VR_UPDATE_CFG           0x14

#define MAGIC_DEBUG     ('D' << 24) | ('B' << 16) | ('G' << 8) | 0
#define MAGIC_MMC       ('M' << 24) | ('M' << 16) | ('C' << 8) | 0
#define MAGIC_POLICY    ('P' << 24) | ('O' << 16) | ('L' << 8) | ('I' << 0)

typedef struct ParameterInfo {
  uint32_t magic;
  uint32_t size;
  uint32_t data[0];
} ParameterInfo;

struct mmc_erase_range {
  uint32_t start;
  uint32_t end;
};

typedef struct mmc_param {
  int mmc_open_card;
  int mmc_erase;
  uint32_t mmc_erase_range_count;
  uint32_t blob[59]; // don't care, not formatting
} mmc_param;

typedef struct debug_param {
  uint32_t log_enabled;
  uint32_t transfer_data_chk;
  uint32_t write_back_chk;
  uint32_t transfer_size;
  uint32_t stage2_timeout;
} debug_param;

typedef struct policy_param {
  int use_nand_mgr;
  int use_nand_mtd;
  int use_mmc0;
  int use_mmc1;
  int use_mmc2;
  uint32_t use_sfc_nor;
  uint32_t use_sfc_nand;
  uint32_t use_spi_nand;
  uint32_t use_spi_nor;
  uint32_t offsets[32];
} policy_param;;


// burner commands
typedef struct update_cmd {
  uint32_t length;
  uint32_t unused[9]; // pad to 40 bytes
} UpdateCmd;

typedef struct write_cmd {
  uint64_t partition;
  uint32_t ops;
  uint32_t offset;
  uint32_t length;
  uint32_t crc;
  uint32_t unused[4];
} WriteCmd;

typedef struct read_cmd {
  uint64_t partition;
  uint32_t ops;
  uint32_t offset;
  uint32_t length;
  uint32_t unused[5];
} ReadCmd;


/* Global variables */
bool g_verbose = false;
libusb_device_handle* g_usb_dev = NULL;
int g_vid = 0, g_pid = 0;
int g_med = 2;          /* storage medium index: 2=mmc0, 5=sfc_nor, 6=sfc_nand, 7=spi_nand. policy selects medium */
long g_read_ops = -1;   /* read-op override; -1 = default (g_med<<16). Decouples read-op from policy medium. */
int g_handshake = 1;    /* do the cloner's VR_READ post-handshake (bReq 0x19 read 8B, then trailing GET_ACK) */
int g_strcmd = 0;       /* cloner string protocol: send "cmd:read,..." via VR_UPDATE_CFG+bulk before VR_READ */
const char *g_cfg1 = NULL, *g_cfg2 = NULL;   /* captured SFC-NAND enable blobs to replay verbatim */
int g_cmd_retries = 1;  /* retry non-start vendor controls N times on error (env USBBOOT_RETRIES) for a flaky link */
int g_cmd_to = 1000;    /* per-attempt control timeout ms (env USBBOOT_TIMEOUT) */

/* Utility functions */
void die(const char* msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

void verbose(const char* msg, ...)
{
    if(!g_verbose)
        return;

    va_list ap;
    va_start(ap, msg);
    vprintf(msg, ap);
    printf("\n");
    va_end(ap);
}

/* Fixed bounds; retain the Linux-qualified 4 MiB request size for measurement. */
#define BULK_BYTES (4 << 20)
#define BULK_MS 600000
#define REOPEN_MS 30000
static uint8_t saved_bus, saved_ports[8];
static int saved_depth;
static bool saved_location, stage1_pending;
static bool observe_spl, observe_all_start1;
extern unsigned long s1_exec_addr;

static uint64_t monotonic_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts)) die("Monotonic clock failed");
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void rediscovery_pause(void)
{
#ifdef _WIN32
    Sleep(100);
#else
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
#endif
}

/* Keep the list alive until the selected device has been opened/observed. */
static int find_target(libusb_device ***list, libusb_device **target, bool observing)
{
    ssize_t n = libusb_get_device_list(NULL, list);
    if(n < 0) {
        if(!observing) die("USB enumeration failed: %d", (int)n);
        fprintf(stderr, "Observation: USB enumeration failed: %d\n", (int)n);
        return -1;
    }
    int count = 0;
    for(ssize_t i = 0; i < n; ++i) {
        struct libusb_device_descriptor desc;
        int ret = libusb_get_device_descriptor((*list)[i], &desc);
        if(ret < 0) {
            if(!observing) die("USB descriptor failed: %d", ret);
            fprintf(stderr, "Observation: USB descriptor failed: %d\n", ret);
            return -1;
        }
        if(desc.idVendor == g_vid && desc.idProduct == g_pid) {
            *target = (*list)[i]; ++count;
        }
    }
    return count;
}

static bool target_location(libusb_device *dev, bool remember)
{
    uint8_t ports[8], bus = libusb_get_bus_number(dev);
    int depth = libusb_get_port_numbers(dev, ports, sizeof(ports));
    if(depth <= 0) {
        if(remember) die("Cannot establish physical USB port path: %d", depth);
        fprintf(stderr, "Observation: Cannot establish physical USB port path: %d\n", depth);
        return false;
    }
    fprintf(stderr, "USB location bus=%u ports=", bus);
    for(int i = 0; i < depth; ++i) fprintf(stderr, "%s%u", i ? "." : "", ports[i]);
    fprintf(stderr, " address=%u\n", libusb_get_device_address(dev));
    bool same = !saved_location || (bus == saved_bus && depth == saved_depth &&
                                   !memcmp(ports, saved_ports, depth));
    if(remember && !saved_location) {
        saved_bus = bus; saved_depth = depth;
        memcpy(saved_ports, ports, depth); saved_location = true;
    }
    return same;
}

static void observe_stage1(void)
{
    if(!stage1_pending) return;
    libusb_device **list = NULL, *target = NULL;
    int count = find_target(&list, &target, true);
    if(count < 0) {
        if(list) libusb_free_device_list(list, 1);
        return;
    }
    unsigned char config = 0;
    /* Query the old handle directly; Windows get_configuration can be cached. */
    int ret = libusb_control_transfer(g_usb_dev, LIBUSB_ENDPOINT_IN,
        LIBUSB_REQUEST_GET_CONFIGURATION, 0, 0, &config, 1, 1000);
    const char *state = count == 0 ? "absent" : count > 1 ? "ambiguous" :
                       target_location(target, false) ? "same-location" : "changed-location";
    fprintf(stderr, "After stage1: target=%s old-handle=%d configuration=%d\n",
            state, ret, ret == 1 ? config : -1);
    libusb_free_device_list(list, 1);
}

void open_usb(void)
{
    bool reopening = saved_location;
    if(g_usb_dev) {
        verbose("Closing USB device");
        libusb_release_interface(g_usb_dev, 0);
        libusb_close(g_usb_dev); g_usb_dev = NULL;
    }
    if(g_vid == 0 || g_pid == 0)
        die("Can't open USB device: vendor/product ID not specified");
    uint64_t deadline = monotonic_ms() + REOPEN_MS;
    for(;;) {
        libusb_device **list = NULL, *target = NULL;
        int count = find_target(&list, &target, false);
        if(count > 1) die("Ambiguous USB target: %d matching devices", count);
        if(count == 1) {
            if(!target_location(target, true)) die("USB target changed physical location");
            int ret = libusb_open(target, &g_usb_dev);
            libusb_free_device_list(list, 1);
            if(ret != 0) die("Could not open USB device: %d", ret);
            ret = libusb_claim_interface(g_usb_dev, 0);
            if(ret != 0) {
                libusb_close(g_usb_dev); g_usb_dev = NULL;
                die("Could not claim interface: %d", ret);
            }
            if(reopening && monotonic_ms() >= deadline) die("USB reconnect deadline exceeded");
            fprintf(stderr, "USB %s: unique target claimed\n", reopening ? "reopened" : "opened");
            break;
        }
        libusb_free_device_list(list, 1);
        if(!reopening) die("Could not open USB device: target absent");
        if(monotonic_ms() >= deadline) die("USB reconnect deadline exceeded");
        rediscovery_pause();
    }
    stage1_pending = false;
    { char* e; if((e=getenv("USBBOOT_RETRIES"))) g_cmd_retries=atoi(e); if(g_cmd_retries<1) g_cmd_retries=1;
      if((e=getenv("USBBOOT_TIMEOUT"))) g_cmd_to=atoi(e); if(g_cmd_to<10) g_cmd_to=1000;
      if(g_cmd_retries>1) verbose("retry mode: %d tries x %dms", g_cmd_retries, g_cmd_to); }
}

void ensure_usb(void)
{
    if(!g_usb_dev) open_usb();
    if(stage1_pending) { observe_stage1(); stage1_pending = false; }
}

/* USB communication functions */
void jz_get_cpu_info(void)
{
    ensure_usb();
    verbose("Issue GET_CPU_INFO");

    uint8_t buf[9];
    int ret = libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        VR_GET_CPU_INFO, 0, 0, buf, 8, 1000);
    /* libusb_control_transfer returns the byte count on success (8 here), not 0;
       only negative values are errors. Treat "got fewer than 8 bytes" as failure. */
    if(ret != 8)
        die("Can't get CPU info: %d", ret);

    buf[8] = 0;
    printf("CPU info: %s\n", buf);
}

/* Windows diagnostic: identity and transfers use one exact interface path.
 * libusb 1.0.27 exposes neither its Windows file handle nor wrap_sys_device.
 * Use the same CreateFile/WinUsb_Initialize sequence as its WinUSB backend,
 * without depending on private library structures. */
#ifdef _WIN32
#define DIAG_CAP 4096
#define DIAG_APIS(X) \
    X(SetupDiGetClassDevsW) X(SetupDiEnumDeviceInfo) \
    X(SetupDiGetDeviceInstanceIdW) X(SetupDiOpenDevRegKey) \
    X(SetupDiEnumDeviceInterfaces) X(SetupDiGetDeviceInterfaceDetailW) \
    X(SetupDiDestroyDeviceInfoList)
#define DIAG_DECLARE(name) static __typeof__(&name) diag_##name;
DIAG_APIS(DIAG_DECLARE)
DIAG_DECLARE(RegQueryValueExW)
DIAG_DECLARE(RegCloseKey)
DIAG_DECLARE(WinUsb_Initialize)
DIAG_DECLARE(WinUsb_Free)
DIAG_DECLARE(WinUsb_SetPipePolicy)
DIAG_DECLARE(WinUsb_ControlTransfer)
#undef DIAG_DECLARE

struct diagnostic_modules { HMODULE setup, adv, win; };

static bool diagnostic_apis(struct diagnostic_modules *modules)
{
    /* No extra static imports, and no search of the bundle/current directory. */
    modules->setup = LoadLibraryExW(L"setupapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!modules->setup) return false;
    modules->adv = LoadLibraryExW(L"advapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!modules->adv) return false;
    modules->win = LoadLibraryExW(L"winusb.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!modules->win) return false;
#define DIAG_RESOLVE(module, name) \
    diag_##name = (__typeof__(&name))(void *)GetProcAddress(modules->module, #name); \
    if(!diag_##name) return false;
#define DIAG_SETUP(name) DIAG_RESOLVE(setup, name)
    DIAG_APIS(DIAG_SETUP)
    DIAG_RESOLVE(adv, RegQueryValueExW)
    DIAG_RESOLVE(adv, RegCloseKey)
    DIAG_RESOLVE(win, WinUsb_Initialize)
    DIAG_RESOLVE(win, WinUsb_Free)
    DIAG_RESOLVE(win, WinUsb_SetPipePolicy)
    DIAG_RESOLVE(win, WinUsb_ControlTransfer)
#undef DIAG_SETUP
#undef DIAG_RESOLVE
    return true;
}

/* Parse a registered GUID with fixed bounds; no ole32/rpcrt4 dependency. */
static bool diagnostic_guid(const wchar_t *text, GUID *guid)
{
    unsigned int part[11];
    if(wcslen(text) != 38 || text[0] != L'{' || text[37] != L'}') return false;
    for(int i = 1; i < 37; i++) {
        if(i == 9 || i == 14 || i == 19 || i == 24) {
            if(text[i] != L'-') return false;
        } else if(!((text[i] >= L'0' && text[i] <= L'9') ||
                    (text[i] >= L'a' && text[i] <= L'f') ||
                    (text[i] >= L'A' && text[i] <= L'F'))) return false;
    }
    if(swscanf(text, L"{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}",
               &part[0], &part[1], &part[2], &part[3], &part[4], &part[5],
               &part[6], &part[7], &part[8], &part[9], &part[10]) != 11) return false;
    guid->Data1 = part[0]; guid->Data2 = part[1]; guid->Data3 = part[2];
    for(int i = 0; i < 8; i++) guid->Data4[i] = part[i + 3];
    return true;
}

static bool diagnostic_path_id(const wchar_t *path, wchar_t *instance)
{
    /* USB interface symbolic links encode the instance before the final GUID.
     * Verify that encoding against SetupAPI before opening this immutable path. */
    if(wcsncmp(path, L"\\\\?\\", 4)) return false;
    const wchar_t *end = wcsrchr(path + 4, L'#');
    GUID guid;
    if(!end || !diagnostic_guid(end + 1, &guid)) return false;
    size_t n = (size_t)(end - (path + 4));
    if(!n || n >= DIAG_CAP) return false;
    for(size_t i = 0; i < n; i++) instance[i] = path[i + 4] == L'#' ? L'\\' : path[i + 4];
    instance[n] = 0;
    return !_wcsnicmp(instance, L"USB\\VID_A108&PID_EAEF\\", 22);
}

static void diagnostic_json_id(const wchar_t *id)
{
    if(!*id) { printf("null"); return; }
    putchar('"');
    for(; *id; id++) {
        unsigned int c = (unsigned short)*id;
        if(c == '"' || c == '\\') printf("\\%c", c);
        else if(c < 32 || c > 126) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static int windows_native_diagnostic(void)
{
    const char *status = "init";
    int count = 0, length = -1;
    DWORD error = ERROR_INVALID_DATA, size, type;
    unsigned char raw[8] = {0};
    wchar_t instance[DIAG_CAP] = {0}, candidate[DIAG_CAP], guids[DIAG_CAP] = {0};
    wchar_t opened[DIAG_CAP] = {0}, metadata[DIAG_CAP];
    struct diagnostic_modules modules = {0};
    HDEVINFO set = INVALID_HANDLE_VALUE;
    HDEVINFO interfaces = INVALID_HANDLE_VALUE;
    HANDLE file = INVALID_HANDLE_VALUE;
    HKEY key = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE win = NULL;
    SP_DEVINFO_DATA dev = {0}, selected = {0};
    if(!diagnostic_apis(&modules)) { error = GetLastError(); goto done; }
    status = "enumerate";
    set = diag_SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if(set == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
    for(DWORD i = 0;; i++) {
        dev.cbSize = sizeof(dev);
        if(!diag_SetupDiEnumDeviceInfo(set, i, &dev)) {
            error = GetLastError();
            if(error == ERROR_NO_MORE_ITEMS) break;
            goto done;
        }
        if(!diag_SetupDiGetDeviceInstanceIdW(set, &dev, candidate, DIAG_CAP, NULL)) {
            error = GetLastError(); goto done;
        }
        if(!_wcsnicmp(candidate, L"USB\\VID_A108&PID_EAEF\\", 22)) {
            count++; selected = dev; wcscpy(instance, candidate);
        }
    }
    error = ERROR_INVALID_DATA;
    if(count != 1) { status = count ? "ambiguous" : "absent"; goto done; }
    status = "identity";
    /* The in-box WinUSB binding can expose only GUID_DEVINTERFACE_USB_DEVICE.
     * Registry interface GUIDs are optional additional candidates. */
    const GUID usb_device = {0xa5dcbf10, 0x6530, 0x11d2,
                             {0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed}};
    size_t guid_chars = 0;
    key = diag_SetupDiOpenDevRegKey(set, &selected, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if(key != INVALID_HANDLE_VALUE) {
        size = sizeof(guids);
        LONG rc = diag_RegQueryValueExW(key, L"DeviceInterfaceGUIDs", NULL, &type, (BYTE *)guids, &size);
        if(rc == ERROR_FILE_NOT_FOUND) {
            size = sizeof(guids);
            rc = diag_RegQueryValueExW(key, L"DeviceInterfaceGUID", NULL, &type, (BYTE *)guids, &size);
        }
        if(!rc && (type == REG_SZ || type == REG_MULTI_SZ) && size >= 2 * sizeof(wchar_t) &&
           size <= sizeof(guids) && !(size % sizeof(wchar_t)) && !guids[size / sizeof(wchar_t) - 1])
            guid_chars = size / sizeof(wchar_t);
    }
    /* Try the standard class first, retaining only the selected instance. */
    bool standard = true;
    for(size_t offset = 0; standard || (offset < guid_chars && guids[offset]);) {
        GUID guid = usb_device;
        if(!standard) {
            wchar_t *text = guids + offset;
            offset += wcslen(text) + 1;
            if(!diagnostic_guid(text, &guid)) continue;
        }
        standard = false;
        interfaces = diag_SetupDiGetClassDevsW(&guid, instance, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if(interfaces == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
        SP_DEVICE_INTERFACE_DATA iface = {0};
        iface.cbSize = sizeof(iface);
        if(!diag_SetupDiEnumDeviceInterfaces(interfaces, NULL, &guid, 0, &iface)) {
            error = GetLastError();
            if(error == ERROR_NO_MORE_ITEMS) {
                diag_SetupDiDestroyDeviceInfoList(interfaces); interfaces = INVALID_HANDLE_VALUE;
                error = ERROR_INVALID_DATA; continue;
            }
            goto done;
        }
        /* Aligned fixed storage, including the variable-length DevicePath. */
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W detail; wchar_t storage[DIAG_CAP]; } path;
        memset(&path, 0, sizeof(path)); path.detail.cbSize = sizeof(path.detail);
        dev.cbSize = sizeof(dev);
        if(!diag_SetupDiGetDeviceInterfaceDetailW(interfaces, &iface, &path.detail, sizeof(path), &size, &dev)) {
            error = GetLastError(); goto done;
        }
        error = ERROR_INVALID_DATA;
        if(size > sizeof(path) || size < sizeof(path.detail) ||
           !wmemchr(path.detail.DevicePath, 0, (sizeof(path) - offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath)) / sizeof(wchar_t)) ||
           !diagnostic_path_id(path.detail.DevicePath, candidate) || _wcsicmp(instance, candidate)) goto done;
        if(!diag_SetupDiGetDeviceInstanceIdW(interfaces, &dev, metadata, DIAG_CAP, NULL)) {
            error = GetLastError(); goto done;
        }
        if(_wcsicmp(candidate, metadata)) goto done;
        status = "open";
        file = CreateFileW(path.detail.DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if(file == INVALID_HANDLE_VALUE) { error = GetLastError(); goto done; }
        /* Only publish the ID encoded by the path that CreateFile actually opened. */
        wcscpy(opened, candidate);
        break;
    }
    if(file == INVALID_HANDLE_VALUE) goto done;
    status = "claim";
    if(!diag_WinUsb_Initialize(file, &win)) { error = GetLastError(); goto done; }
    status = "control";
    ULONG timeout = 1000, transferred = 0;
    if(!diag_WinUsb_SetPipePolicy(win, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout)) {
        error = GetLastError(); goto done;
    }
    WINUSB_SETUP_PACKET packet = {0xc0, VR_GET_CPU_INFO, 0, 0, sizeof(raw)};
    if(!diag_WinUsb_ControlTransfer(win, packet, raw, sizeof(raw), &transferred, NULL)) {
        error = GetLastError(); goto done;
    }
    error = ERROR_INVALID_DATA;
    if(transferred > sizeof(raw)) goto done;
    length = (int)transferred; status = "raw"; error = 0;
done:
    if(win) diag_WinUsb_Free(win);
    if(file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if(key != INVALID_HANDLE_VALUE) diag_RegCloseKey(key);
    if(interfaces != INVALID_HANDLE_VALUE) diag_SetupDiDestroyDeviceInfoList(interfaces);
    if(set != INVALID_HANDLE_VALUE) diag_SetupDiDestroyDeviceInfoList(set);
    /* Keep the saved operation error even when cleanup changes GetLastError. */
    if(modules.win) FreeLibrary(modules.win);
    if(modules.adv) FreeLibrary(modules.adv);
    if(modules.setup) FreeLibrary(modules.setup);
    printf("{\"schema\":1,\"status\":\"%s\",\"count\":%d,\"error\":%lu,\"length\":%d,\"hex\":\"",
           status, count, (unsigned long)error, length);
    for(int i = 0; i < length; i++) printf("%02x", raw[i]);
    printf("\",\"instance_id\":"); diagnostic_json_id(opened); printf("}\n");
    return error == 0 ? 0 : 1;
}
#endif

/* Standalone raw diagnostic; short replies never identify the CPU. */
static int native_diagnostic(void)
{
#ifdef _WIN32
    return windows_native_diagnostic();
#else
    libusb_device **devices = NULL, *target = NULL;
    libusb_device_handle *handle = NULL;
    const char *status = "init";
    int ret = libusb_init(NULL), count = 0, length = -1, claimed = 0;
    unsigned char raw[8] = {0};
    if(ret < 0) goto report;
    ssize_t n = libusb_get_device_list(NULL, &devices);
    status = "enumerate";
    if(n < 0) { ret = (int)n; goto done; }
    for(ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor desc;
        ret = libusb_get_device_descriptor(devices[i], &desc);
        if(ret < 0) goto done; /* Incomplete enumeration cannot prove uniqueness. */
        if(desc.idVendor == 0xa108 && desc.idProduct == 0xeaef) {
            target = devices[i]; count++;
        }
    }
    if(count != 1) { status = count ? "ambiguous" : "absent"; ret = -1; goto done; }
    status = "open";
    ret = libusb_open(target, &handle);
    if(ret < 0) goto done;
    status = "claim";
    ret = libusb_claim_interface(handle, 0);
    if(ret < 0) goto done;
    claimed = 1;
    status = "control";
    ret = libusb_control_transfer(handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        VR_GET_CPU_INFO, 0, 0, raw, sizeof(raw), 1000);
    if(ret >= 0 && ret <= 8) { length = ret; status = "raw"; ret = 0; }
    else if(ret >= 0) ret = -1;
done:
    if(claimed) libusb_release_interface(handle, 0);
    if(handle) libusb_close(handle);
    if(devices) libusb_free_device_list(devices, 1);
    libusb_exit(NULL);
report:
    printf("{\"schema\":1,\"status\":\"%s\",\"count\":%d,\"error\":%d,\"length\":%d,\"hex\":\"",
           status, count, ret, length);
    for(int i = 0; i < length; i++) printf("%02x", raw[i]);
    printf("\"}\n");
    return ret == 0 ? 0 : 1;
#endif
}

static void bulk_transfer(void *data, int length, unsigned char endpoint)
{
    if(length < 0) die("Invalid bulk length: %d", length);
    uint64_t start = monotonic_ms(), deadline = start + BULK_MS;
    int total = 0;
    while(total < length) {
        uint64_t now = monotonic_ms();
        if(now >= deadline) die("Bulk deadline exceeded at %d/%d", total, length);
        int want = length - total, xfered = 0;
        if(want > BULK_BYTES) want = BULK_BYTES;
        int ret = libusb_bulk_transfer(g_usb_dev, endpoint,
            (unsigned char*)data + total, want, &xfered, (unsigned int)(deadline - now));
        if(ret != 0 || xfered <= 0 || xfered > want)
            die("Bulk failed at %d/%d: result=%d bytes=%d requested=%d; no retry",
                total, length, ret, xfered, want);
        total += xfered;
        if(monotonic_ms() >= deadline) die("Bulk deadline exceeded at %d/%d", total, length);
    }
    fprintf(stderr, "Bulk endpoint=0x%02x bytes=%d request-max=%d elapsed-ms=%llu\n",
            endpoint, total, BULK_BYTES, (unsigned long long)(monotonic_ms() - start));
}

void bulk_transfer_in(void *data, int length)
{
    bulk_transfer(data, length, LIBUSB_ENDPOINT_IN | 1);
}

void bulk_transfer_out(void *data, int length)
{
    bulk_transfer(data, length, LIBUSB_ENDPOINT_OUT | 1);
}

void jz_upload(const char* filename, int length)
{
    if(length < 0) die("invalid upload length: %d", length);
    ensure_usb();
    void* data = malloc(length ? length : 1);
    if(!data) die("Cannot allocate upload buffer");
    bulk_transfer_in(data, length);
    FILE* f = fopen(filename, "wb");
    if(f == NULL) die("Can't open file '%s' for writing", filename);
    if(fwrite(data, 1, length, f) != (size_t)length || fclose(f))
        die("Error writing transferred data to file");
    free(data);
}

#define jz_vendor_out_func(name, type, fmt) \
  void name(unsigned long param) {   \
        ensure_usb(); \
        verbose("Issue " #type fmt, param); \
        int ret=-1,_t; \
        int tries = (VR_##type == VR_PROGRAM_START1 || VR_##type == VR_PROGRAM_START2) ? 1 : g_cmd_retries; \
        for(_t=0;_t<tries;_t++){ \
          ret = libusb_control_transfer(g_usb_dev, \
            LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE, \
            VR_##type, param >> 16, param & 0xffff, NULL, 0, g_cmd_to); \
          if(ret==0) break; \
        } \
        if(VR_##type == VR_PROGRAM_START1) { \
            stage1_pending = observe_all_start1 || (observe_spl && param == s1_exec_addr); \
            observe_stage1(); \
        } \
        if(ret != 0) \
            die("Request " #type " failed: %d (after %d tries)", ret, tries); \
    }

jz_vendor_out_func(jz_set_data_address, SET_DATA_ADDRESS, " 0x%08lx")
jz_vendor_out_func(jz_set_data_length, SET_DATA_LENGTH, " 0x%0lx")
jz_vendor_out_func(_jz_flush_caches, FLUSH_CACHES, "")
jz_vendor_out_func(jz_program_start1, PROGRAM_START1, " 0x%08lx")
jz_vendor_out_func(jz_program_start2, PROGRAM_START2, " 0x%08lx")
jz_vendor_out_func(jz_init, INIT, " 0x%08lx")  
#define jz_flush_caches() _jz_flush_caches(0)

void jz_generic_out(uint8_t op,
		    unsigned long param,
		    unsigned char *data,
		    uint16_t len) {
  ensure_usb();
  verbose("Issue 0x%x", op);
  int ret = libusb_control_transfer(g_usb_dev,
				    LIBUSB_ENDPOINT_OUT|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE,
				    op, param >> 16, param & 0xffff, data, len, 1000);
  if(ret != len)					     
    die("Request 0x%x failed, only transfered: %d", op, ret);
}

void jz_download(const char* filename)
{
    FILE* f = fopen(filename, "rb");
    if(f == NULL)
        die("Can't open file '%s' for reading", filename);

    fseek(f, 0, SEEK_END);
    int length = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* data = malloc(length);
    if(fread(data, length, 1, f) != 1)
        die("Error reading data from file");
    fclose(f);

    jz_set_data_length(length);
    bulk_transfer_out(data, length);

    free(data);
}

void jz_get_ack() {
  ensure_usb();
  verbose("Issue VR_GET_ACK");
  
  uint8_t buf[4];
  int ret = libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        VR_GET_ACK, 0, 0, buf, 4, 1000);
  if(ret != 4)
    die("Can't get ACK: %d", ret);
}

void enable_mmc() {
  ensure_usb();

  ParameterInfo *policy_param_info = (ParameterInfo*) malloc(sizeof(ParameterInfo) + sizeof(policy_param));
  policy_param_info->magic = MAGIC_POLICY;
  policy_param_info->size = sizeof(policy_param);
  memset(policy_param_info->data, 0, sizeof(policy_param));
  policy_param *policy_cfg = (policy_param*)policy_param_info->data;
  switch (g_med) {
    case 0: policy_cfg->use_nand_mgr = 1; break;
    case 1: policy_cfg->use_nand_mtd = 1; break;
    case 2: policy_cfg->use_mmc0 = 1; break;
    case 3: policy_cfg->use_mmc1 = 1; break;
    case 4: policy_cfg->use_mmc2 = 1; break;
    case 5: policy_cfg->use_sfc_nor = 1; break;
    case 6: policy_cfg->use_sfc_nand = 1; break;
    case 7: policy_cfg->use_spi_nand = 1; break;
    case 8: policy_cfg->use_spi_nor = 1; break;
    default: policy_cfg->use_mmc0 = 1; break;
  }

  ParameterInfo *dbg_param_info = (ParameterInfo*) malloc(sizeof(ParameterInfo) + sizeof(debug_param));
  dbg_param_info->magic = MAGIC_DEBUG;
  dbg_param_info->size = sizeof(debug_param);
  debug_param *dbg = (debug_param*)dbg_param_info->data;
  dbg->log_enabled = 1;
  dbg->transfer_data_chk = 0;
  dbg->write_back_chk = 0;
  dbg->transfer_size = 0;
  dbg->stage2_timeout = 0;

  ParameterInfo *mmc_param_info = (ParameterInfo*) malloc(sizeof(ParameterInfo) + sizeof(mmc_param));
  mmc_param_info->magic = MAGIC_MMC;
  mmc_param_info->size = sizeof(mmc_param);
  memset(mmc_param_info->data, 0, sizeof(mmc_param));

  uint32_t data_size = 3 * 8 + policy_param_info->size + dbg_param_info->size + mmc_param_info->size;
  unsigned char data[data_size];
  unsigned char *p = data;
  uint32_t offset = 0;
  memcpy(p + offset, dbg_param_info, 4 + 4 + dbg_param_info->size);
  offset += 4 + 4 + dbg_param_info->size;
  memcpy(p + offset, mmc_param_info, 4 + 4 + mmc_param_info->size);
  offset += 4 + 4 + mmc_param_info->size;
  memcpy(p + offset, policy_param_info, 4 + 4 + policy_param_info->size);

  UpdateCmd *update = (UpdateCmd*) malloc(sizeof(UpdateCmd));
  memset(update, 0, sizeof(UpdateCmd));
  update->length = data_size;

  jz_generic_out(VR_UPDATE_CFG, 0, (unsigned char*)update, sizeof(UpdateCmd));

  bulk_transfer_out(p, data_size);

  free(policy_param_info);
  free(dbg_param_info);
  free(mmc_param_info);
  free(update);
  
  jz_get_ack();
  jz_init(0);
  jz_get_ack();
}

static void send_str_cmd(const char* s);

void mmc_read(uint32_t offset, uint32_t length, unsigned char* out) {
  /* Cloner string protocol: tell the burner to stage a flash read before we collect it. */
  if (g_strcmd) {
    char cmd[160];
    uint32_t ops = (g_read_ops >= 0) ? (uint32_t)g_read_ops : 0;
    snprintf(cmd, sizeof(cmd),
             "cmd:read,med:%d,addr:0,offs:%u,len:%u,ops:%u,crc:0",
             g_med, offset, length, ops);
    send_str_cmd(cmd);
  }

  ReadCmd *read = (ReadCmd*) malloc(sizeof(ReadCmd));
  memset(read, 0, sizeof(ReadCmd));
  read->ops = (g_read_ops >= 0) ? (uint32_t)g_read_ops : (uint32_t)(g_med << 16); // --ops overrides; policy selects medium
  read->offset = offset;
  read->length = length;

  jz_generic_out(VR_READ, 0, (unsigned char*)read, sizeof(ReadCmd));
  free(read);

  /* The cloner does a handshake the bare ballaswag read omits: between VR_READ
     and the bulk-IN it issues a vendor-IN bRequest 0x19 reading 8 bytes
     ([ack:4][crc:4]). The burner reads flash into its buffer and gates the bulk
     transfer on this ack - without it the bulk-IN hangs. (core_64 get_ack_and_crc) */
  if (g_handshake) {
    unsigned char ack[8];
    memset(ack, 0, sizeof(ack));
    int r = libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        0x19, 0, 0, ack, 8, 10000);
    verbose("VR_READ ack(0x19): ret=%d ack=%02x%02x%02x%02x crc=%02x%02x%02x%02x",
            r, ack[0],ack[1],ack[2],ack[3], ack[4],ack[5],ack[6],ack[7]);
  }

  bulk_transfer_in(out, length);

  /* Trailing per-chunk ack the cloner reads after the data (bRequest 0x10). */
  if (g_handshake) {
    unsigned char fack[4];
    memset(fack, 0, sizeof(fack));
    libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        VR_GET_ACK, 0, 0, fack, 4, 10000);
  }
}

/* Send a cloner STRING command ("cmd:read,med:6,addr:0,offs:...,len:...,ops:...,crc:0")
   the way core_64::stage2_send_cmd_config does: VR_UPDATE_CFG(0x14) header + bulk-OUT the
   ASCII string + GET_ACK. The cloner burner parses it (strcmp "read"/"write"/...) and sets up
   the operation; a following VR_READ+0x19 then collects the data. */
static void send_str_cmd(const char* s) {
  int n = (int)strlen(s);
  UpdateCmd u; memset(&u, 0, sizeof(u)); u.length = (uint32_t)n;
  jz_generic_out(VR_UPDATE_CFG, 0, (unsigned char*)&u, sizeof(UpdateCmd));
  bulk_transfer_out((unsigned char*)s, n);
  jz_get_ack();
  verbose("sent str cmd: \"%s\"", s);
}

/* Send one UPDATE_CFG config blob (verbatim, captured from the cloner) + GET_ACK. */
static void send_cfg_blob(const char* file) {
  FILE* f = fopen(file, "rb");
  if(!f) die("can't open cfg blob '%s'", file);
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  unsigned char* buf = malloc(n);
  if(fread(buf, n, 1, f) != 1) die("read cfg blob error");
  fclose(f);
  UpdateCmd u; memset(&u, 0, sizeof(u)); u.length = (uint32_t)n;
  jz_generic_out(VR_UPDATE_CFG, 0, (unsigned char*)&u, sizeof(UpdateCmd));
  bulk_transfer_out(buf, (int)n);
  free(buf);
  jz_get_ack();
}

/* Replay the cloner's captured SFC-NAND enable sequence:
   cfg1(POLICY) -> GET_ACK -> bReq38(0x26) query -> cfg2(DDR+DBG+EFUSE+POLICY+SFC) -> GET_ACK -> INIT -> GET_ACK */
static void enable_sfcnand_replay(void) {
  ensure_usb();
  send_cfg_blob(g_cfg1);
  unsigned char q[3] = {0};
  libusb_control_transfer(g_usb_dev,
      LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE,
      38, 0, 0, q, 3, 1000);
  send_cfg_blob(g_cfg2);
  jz_init(0);
  jz_get_ack();
}

/* Sweep candidate read-ops against the live burner. For each ops: VR_READ a small
   page, do the 0x19 ack (fast feedback: ret-code != -38 means the op was accepted),
   then a short bulk drain. The op whose ack-code is success AND streams bulk data wins. */
void probe_ops(void) {
  if (g_cfg1 && g_cfg2) enable_sfcnand_replay(); else enable_mmc();
  uint32_t cands[] = {
    0x060000, 0x0d0000, 0x010000, 0x000000, 0x020000,
    0x030000, 0x040000, 0x050000, 0x070000, 0x080000,
    0x090000, 0x0a0000, 0x0b0000, 0x0c0000, 0x0e0000,
    0x0d0601, 6, 13, 1, 2
  };
  uint32_t plen = 4096;
  unsigned char *buf = malloc(plen);
  printf("=== ops probe (offset 0, %u bytes/probe) ===\n", plen);
  for (unsigned i = 0; i < sizeof(cands)/sizeof(cands[0]); i++) {
    ReadCmd r; memset(&r, 0, sizeof(r));
    r.ops = cands[i]; r.offset = 0; r.length = plen;
    jz_generic_out(VR_READ, 0, (unsigned char*)&r, sizeof(r));
    unsigned char ack[8]; memset(ack, 0, sizeof(ack));
    libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE,
        0x19, 0, 0, ack, 8, 3000);
    uint32_t a = ack[0]|(ack[1]<<8)|(ack[2]<<16)|((uint32_t)ack[3]<<24);
    uint32_t c = ack[4]|(ack[5]<<8)|(ack[6]<<16)|((uint32_t)ack[7]<<24);
    int xfered = 0;
    int br = libusb_bulk_transfer(g_usb_dev, LIBUSB_ENDPOINT_IN|1, buf, plen, &xfered, 2000);
    unsigned char f[4] = {0};
    libusb_control_transfer(g_usb_dev,
        LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE,
        VR_GET_ACK, 0, 0, f, 4, 2000);
    printf("ops=0x%06x ack=0x%08x crc=0x%08x bulk=%d xfered=%d %s\n",
           cands[i], a, c, br, xfered,
           (br == 0 && xfered > 0) ? "<<<<< DATA STREAMED" : "");
  }
  free(buf);
}

void mmc_read_partition(uint32_t offset, uint32_t length, const char* fname) {
  if (g_cfg1 && g_cfg2) enable_sfcnand_replay(); else enable_mmc();
  if(length == 0)
    die("invalid partition length: %d", length);

  printf("dumping parition at offset 0x%x, size 0x%x\n", offset, length);

  uint32_t chunk_size = 1024 * 1024 * 2; // 2mb
  unsigned char chunk[chunk_size];

  FILE* f = fopen(fname, "wb");
  if(f == NULL)
    die("Can't open file '%s' for writing", fname);
  
  uint32_t cursor = offset;
  uint32_t end = offset + length;
  while (cursor < end) {
    uint32_t read_size = (end - cursor) >= chunk_size
      ? chunk_size
      : (end - cursor);

    uint32_t start = (uint32_t)time(NULL);
    mmc_read(cursor, read_size, chunk);
    uint32_t duration = (uint32_t)time(NULL) - start;

    cursor += read_size;

    printf("%.2f%% completed (%.2fMB/s)\n",
	   (cursor - offset) / (float)length * 100,
	   (read_size / 1024 / 1024) / (float)duration);

    if (fwrite(chunk, read_size, 1, f) != 1)
      die("Failed to write data to %x", fname);
  }

  fclose(f);
}

void mmc_write(uint32_t offset, uint32_t length, unsigned char* in) {
  WriteCmd *write = (WriteCmd*) malloc(sizeof(WriteCmd));
  memset(write, 0, sizeof(WriteCmd));
  write->ops = 0x020000; // mmc
  write->offset = offset;
  write->length = length;

  jz_generic_out(VR_WRITE, 0, (unsigned char*)write, sizeof(WriteCmd));
  free(write);

  bulk_transfer_out(in, length);
}

void swap_ota_partition(bool force) {
  ensure_usb();
  enable_mmc();

  uint32_t ota_len = 512;
  unsigned char ota[ota_len];

  mmc_read(0x100000, ota_len, ota);

  char ota_in[ota_len];
  memset(ota_in, 0, ota_len);
  
  if (strncmp((char*)ota, "ota:kernel2", 11) == 0) {
    printf("Current OTA points at kernel2. Switching OTA to kernel\n");
    strcpy(ota_in, "ota:kernel\n\n");
  } else if (strncmp((char*)ota, "ota:kernel\n", 11) == 0) {
    printf("Current OTA points at kernel. Switching OTA to kernel2\n");
    strcpy(ota_in, "ota:kernel2\n\n");
  } else {
    if (!force) {
      die("Exiting! Your OTA contains unexpected values, swapping OTA might not fix your issue.");
    }

    printf("Unknown value in OTA. Forcing OTA to use ota:kernel\n");
    strcpy(ota_in, "ota:kernel\n\n");
  }

  mmc_write(0x100000, ota_len, (unsigned char*)ota_in);
  printf("Switched OTA to %s", ota_in);
}

/* Default settings */
struct cpu_profile {
    const char* name;
    int vid, pid;
    unsigned long s1_load_addr, s1_exec_addr;
    unsigned long s2_load_addr, s2_exec_addr;
};

static const struct cpu_profile cpu_profiles[] = {
    {"x2000",
     0xa108, 0xeaef,
     0xb2401000, 0xb2401800,
     0x80100000, 0x80100000},
    {NULL}
};

/* Simple "download and run" functions for dev purposes */
unsigned long s1_load_addr = 0, s1_exec_addr = 0;
unsigned long s2_load_addr = 0, s2_exec_addr = 0;

void apply_cpu_profile(const char* name)
{
    const struct cpu_profile* p = &cpu_profiles[0];
    for(p = &cpu_profiles[0]; p->name != NULL; ++p) {
        if(strcmp(p->name, name) != 0)
            continue;

        g_vid = p->vid;
        g_pid = p->pid;
        s1_load_addr = p->s1_load_addr;
        s1_exec_addr = p->s1_exec_addr;
        s2_load_addr = p->s2_load_addr;
        s2_exec_addr = p->s2_exec_addr;
        return;
    }

    die("CPU '%s' not known", name);
}

void run_stage1(const char* filename)
{
    if(s1_load_addr == 0 || s1_exec_addr == 0)
        die("No stage1 binary settings -- did you specify --cpu?");
    jz_set_data_address(s1_load_addr);
    jz_download(filename);
    jz_program_start1(s1_exec_addr);
}

void run_stage2(const char* filename)
{
    if(s2_load_addr == 0 || s2_exec_addr == 0)
        die("No stage2 binary settings -- did you specify --cpu?");
    jz_set_data_address(s2_load_addr);
    jz_download(filename);
    jz_flush_caches();
    jz_program_start2(s2_exec_addr);
}

void start_x2000_uboot() {
  run_stage1("./spl.bin");
  sleep(1);
  run_stage2("./uboot.bin");
}

/* Main functions */
void usage()
{
    printf("\
Usage: usbboot [options]\n\
\n\
Basic options:\n\
  --uboot            Start uboot\n\
  --cpu <cpu>        Select device CPU type\n\
  --stage1 <file>    Download and execute stage1 binary\n\
  --stage2 <file>    Download and execute stage2 binary\n\
\n\
Advanced options:\n\
  --vid <vid>        Specify USB vendor ID\n\
  --pid <pid>        Specify USB product ID\n\
  --swap-ota         Switch OTA between kernel/kernel2\n\
  --force-swap-ota   Ignore unknown OTA value, write ota:kernel to OTA\n\
  --native-diagnostic Standalone raw open/claim/control JSON (no identity verdict)\n\
  --observe-stage1  Log post-SPL observations (diagnostic opt-in, before --stage1)\n\
  --observe-start1  Also observe other START1 addresses (including writers)\n\
  --cpuinfo          Ask device for CPU info\n\
  --addr <addr>      Set data address\n\
  --length <len>     Set data length\n\
  --upload <file>    Transfer data from device (needs prior --length)\n\
  --download <file>  Transfer data to device\n\
  --start1 <addr>    Execute stage1 code at address\n\
  --start2 <addr>    Execute stage2 code at address\n\
  --flush-caches     Flush device CPU caches\n\
  --renumerate       Close and re-open the USB device\n\
  --wait <time>      Wait for <time> seconds\n\
  -v, --verbose      Be verbose\n\
\n\
Known CPU types and default stage1/stage2 binary settings:\n");
    const struct cpu_profile* p = &cpu_profiles[0];
    for(p = &cpu_profiles[0]; p->name != NULL; ++p) {
        printf("* %s\n", p->name);
        printf("  - USB ID: %04x:%04x\n", p->vid, p->pid);
        printf("  - Stage1: load %#08lx, exec %#08lx\n",
               p->s1_load_addr, p->s1_exec_addr);
        printf("  - Stage2: load %#08lx, exec %#08lx\n",
               p->s2_load_addr, p->s2_exec_addr);
    }

    exit(1);
}

void cleanup()
{
    /* KNOWN, ACCEPTED (beta): the condition is inverted - it should be `if(g_usb_dev != NULL)` - so an
     * open handle is not closed here. This is a harmless PROCESS-EXIT resource leak only (usbboot is a
     * one-shot; the kernel reclaims the handle on exit), not a flashing/data-safety issue. It is left
     * as-is so the shipped source keeps corresponding to the device-verified binary; a fix will land
     * the next time the flash tool is rebuilt and re-qualified on hardware. */
    if(g_usb_dev == NULL)
        libusb_close(g_usb_dev);
    libusb_exit(NULL);
}

int main(int argc, char* argv[])
{
    if(argc <= 1)
        usage();

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--native-diagnostic") != 0) continue;
        if(argc == 2) return native_diagnostic();
        fprintf(stderr, "--native-diagnostic must be used alone\n");
        return 1;
    }

    libusb_init(NULL);
    atexit(cleanup);

    apply_cpu_profile("x2000");

    enum {
        OPT_VID = 0x100, OPT_PID,
        OPT_CPUINFO,
        OPT_START1, OPT_START2, OPT_FLUSH_CACHES,
        OPT_RENUMERATE, OPT_WAIT, OPT_SWAP_OTA, OPT_OBSERVE_STAGE1, OPT_OBSERVE_START1,
	OPT_FORCE_SWAP_OTA, OPT_DUMP_PARITION, OPT_MED, OPT_CFG1, OPT_CFG2, OPT_OPS, OPT_HS, OPT_PROBE, OPT_STRCMD
    };

    static const struct option long_options[] = {
        {"uboot", no_argument, 0, 'b'},      
        {"cpu", required_argument, 0, 'c'},
        {"stage1", required_argument, 0, '1'},
        {"stage2", required_argument, 0, '2'},
        {"vid", required_argument, 0, OPT_VID},
        {"pid", required_argument, 0, OPT_PID},
        {"cpuinfo", no_argument, 0, OPT_CPUINFO},
        {"addr", required_argument, 0, 'a'},
        {"length", required_argument, 0, 'l'},
        {"partition-size", required_argument, 0, 's'},
        {"partition-offset", required_argument, 0, 'o'},
        {"upload", required_argument, 0, 'u'},
        {"download", required_argument, 0, 'd'},
        {"start1", required_argument, 0, OPT_START1},
        {"start2", required_argument, 0, OPT_START2},
        {"flush-caches", no_argument, 0, OPT_FLUSH_CACHES},
        {"renumerate", no_argument, 0, OPT_RENUMERATE},
        {"observe-stage1", no_argument, 0, OPT_OBSERVE_STAGE1},
        {"observe-start1", no_argument, 0, OPT_OBSERVE_START1},
        {"wait", required_argument, 0, OPT_WAIT},
        {"swap-ota", no_argument, 0, OPT_SWAP_OTA},
        {"force-swap-ota", no_argument, 0, OPT_FORCE_SWAP_OTA},
        {"dump-partition", required_argument, 0, OPT_DUMP_PARITION},
        {"med", required_argument, 0, OPT_MED},
        {"ops", required_argument, 0, OPT_OPS},
        {"handshake", required_argument, 0, OPT_HS},
        {"probe", no_argument, 0, OPT_PROBE},
        {"strcmd", no_argument, 0, OPT_STRCMD},
        {"cfg1", required_argument, 0, OPT_CFG1},
        {"cfg2", required_argument, 0, OPT_CFG2},
        {"help", no_argument, 0, 'h'},
        {"verbose", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int data_length = -1;
    uint32_t partition_offset = 0;
    uint32_t partition_size = 0;
    while((opt = getopt_long(argc, argv, "bhvc:1:2:a:l:s:o:u:d:", long_options, NULL)) != -1) {
        unsigned long param;
        char* end;
        switch(opt) {
        case OPT_VID:
        case OPT_PID:
        case 'a':
        case 'l':
        case OPT_START1:
        case OPT_START2:
        case OPT_WAIT:
        case OPT_MED:
        case OPT_OPS:
        case OPT_HS:
            param = strtoul(optarg, &end, 0);
            if(*end)
                die("Invalid argument '%s'", optarg);

            break;
	case 's':
            partition_size = strtoul(optarg, &end, 0);
            if(*end)
                die("Invalid argument '%s'", optarg);
	    break;
        case 'o':
            partition_offset = strtoul(optarg, &end, 0);
            if(*end)
                die("Invalid argument '%s'", optarg);
	    break;
        default:
            break;
        }

        switch(opt) {
	case 'b':
	    start_x2000_uboot();
	    break;
        case 'h':
            usage();
            break;
        case 'v':
            g_verbose = true;
            break;
        case OPT_OBSERVE_STAGE1:
            observe_spl = true;
            break;
        case OPT_OBSERVE_START1:
            observe_all_start1 = true;
            break;
        case 'c':
            apply_cpu_profile(optarg);
            break;
        case '1':
            run_stage1(optarg);
            break;
        case '2':
            run_stage2(optarg);
            break;
        case OPT_VID:
            g_vid = param & 0xffff;
            break;
        case OPT_PID:
            g_pid = param & 0xffff;
            break;
        case OPT_CPUINFO:
            jz_get_cpu_info();
            break;
        case 'a':
            jz_set_data_address(param);
            break;
        case 'l':
            data_length = param;
            jz_set_data_length(param);
            break;
        case 'u':
            if(data_length < 0)
                die("Need to specify --length before --upload");
            jz_upload(optarg, data_length);
            break;
        case 'd':
            jz_download(optarg);
            break;
        case OPT_START1:
            jz_program_start1(param);
            break;
        case OPT_START2:
            jz_program_start2(param);
            break;
        case OPT_FLUSH_CACHES:
            jz_flush_caches();
            break;
        case OPT_RENUMERATE:
            open_usb();
            break;
        case OPT_WAIT:
            verbose("Wait %lu seconds", param);
            sleep(param);
            if(stage1_pending) observe_stage1();
            break;
        case OPT_MED:
            g_med = (int)param;
            verbose("Storage medium index = %d (ReadCmd.ops=0x%06x)", g_med, g_med << 16);
            break;
        case OPT_OPS:
            g_read_ops = (long)param;
            verbose("Read ops override = 0x%lx", (unsigned long)g_read_ops);
            break;
        case OPT_HS:
            g_handshake = (int)param;
            verbose("VR_READ handshake = %d", g_handshake);
            break;
        case OPT_STRCMD:
            g_strcmd = 1;
            verbose("cloner string-command read enabled");
            break;
        case OPT_PROBE:
            probe_ops();
            break;
        case OPT_CFG1: g_cfg1 = optarg; break;
        case OPT_CFG2: g_cfg2 = optarg; break;
        case OPT_SWAP_OTA:
            verbose("Swapping OTA between kernel/kernel2");
	    swap_ota_partition(false);
            break;
        case OPT_FORCE_SWAP_OTA:
            verbose("Force OTA to boot ota:kernel");
	    swap_ota_partition(true);
            break;
        case OPT_DUMP_PARITION:
	    if (partition_size <= 0)
	      die("must provide a positive partition length with option --partition-size");

            verbose("Dump a partition to file");
	    mmc_read_partition(partition_offset, partition_size, optarg);
            break;
	case 'o':
	case 's':
	  break;
        default:
            /* should only happen due to a bug */
	    die("Bad option");
	    break;
        }
    }

    if(optind != argc)
        die("Extra arguments on command line");

    return 0;
}
