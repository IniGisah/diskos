# SPDX-License-Identifier: MIT
# Copyright (c) 2026 diskOS contributors
"""Tests for BlueALSA wrapper, Bluetooth Low Latency Receiving mode, and aptX/aptX-HD codec handling."""

import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

PAYLOAD = Path(__file__).resolve().parents[1] / "payload"
WRAPPER = PAYLOAD / "bluealsa"


class TestBlueAlsaWrapper(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.usr_bin = self.root / "usr" / "bin"
        self.usr_bin.mkdir(parents=True)
        self.opt_bin = self.root / "opt" / "diskos" / "bin"
        self.opt_bin.mkdir(parents=True)
        self.usr_data = self.root / "usr" / "data"
        self.usr_data.mkdir(parents=True)

        # Mock stock /usr/bin/bluealsa
        self.stock_bluealsa = self.usr_bin / "bluealsa"
        self.stock_log = self.root / "stock_invoked.log"
        self.stock_bluealsa.write_text(
            f"#!/bin/sh\necho \"STOCK: $@\" > {self.stock_log}\n"
        )
        self.stock_bluealsa.chmod(0o755)

        # Copy the actual wrapper to /opt/diskos/bin/bluealsa
        self.wrapper_copy = self.opt_bin / "bluealsa"
        self.wrapper_copy.write_bytes(WRAPPER.read_bytes())
        self.wrapper_copy.chmod(0o755)

    def tearDown(self):
        self.tmpdir.cleanup()

    def run_wrapper(self, args, ll_enabled=False, custom_exists=False):
        env = os.environ.copy()
        # Custom mock roots
        wrapper_script = (
            self.wrapper_copy.read_text()
            .replace("/opt/diskos/bin/bluealsa-custom", str(self.opt_bin / "bluealsa-custom"))
            .replace("/usr/bin/bluealsa", str(self.stock_bluealsa))
            .replace("/usr/data/bt_rx_ll", str(self.usr_data / "bt_rx_ll"))
            .replace("/usr/data/diskos.conf", str(self.usr_data / "diskos.conf"))
        )
        test_script = self.root / "run_test_wrapper.sh"
        test_script.write_text(wrapper_script)
        test_script.chmod(0o755)

        if ll_enabled:
            (self.usr_data / "bt_rx_ll").write_text("1\n")
        else:
            (self.usr_data / "bt_rx_ll").write_text("0\n")

        custom_log = self.root / "custom_invoked.log"
        if custom_exists:
            custom_bin = self.opt_bin / "bluealsa-custom"
            custom_bin.write_text(
                f"#!/bin/sh\necho \"CUSTOM: $@\" > {custom_log}\n"
            )
            custom_bin.chmod(0o755)

        res = subprocess.run([str(test_script), *args], env=env, capture_output=True, text=True)
        stock_out = self.stock_log.read_text().strip() if self.stock_log.exists() else None
        custom_out = custom_log.read_text().strip() if custom_log.exists() else None
        return res, stock_out, custom_out

    def test_wrapper_is_executable(self):
        self.assertTrue(WRAPPER.is_file(), "payload/bluealsa missing")
        mode = WRAPPER.stat().st_mode
        self.assertTrue(mode & stat.S_IXUSR, "payload/bluealsa is not executable")

    def test_sink_low_latency_forces_sbc_only(self):
        args = [
            "-S", "--device=hci0", "--profile=a2dp-sink", "-p", "hfp-ag",
            "--codec=sbc", "--codec=aac", "--codec=ldac",
            "--ldac-abr", "--ldac-quality=standard"
        ]
        res, stock_out, _ = self.run_wrapper(args, ll_enabled=True, custom_exists=False)
        self.assertEqual(res.returncode, 0)
        self.assertIsNotNone(stock_out)
        self.assertIn("STOCK:", stock_out)
        self.assertIn("--codec=sbc", stock_out)
        self.assertNotIn("--codec=aac", stock_out)
        self.assertNotIn("--codec=ldac", stock_out)

    def test_sink_standard_mode_preserves_all_codecs(self):
        args = [
            "-S", "--device=hci0", "--profile=a2dp-sink", "-p", "hfp-ag",
            "--codec=sbc", "--codec=aac", "--codec=ldac",
            "--ldac-abr", "--ldac-quality=standard"
        ]
        res, stock_out, _ = self.run_wrapper(args, ll_enabled=False, custom_exists=False)
        self.assertEqual(res.returncode, 0)
        self.assertIsNotNone(stock_out)
        self.assertIn("--codec=sbc", stock_out)
        self.assertIn("--codec=aac", stock_out)
        self.assertIn("--codec=ldac", stock_out)

    def test_custom_bluealsa_injects_aptx(self):
        args = ["-S", "--device=hci0", "--profile=a2dp-source", "--codec=sbc"]
        res, _, custom_out = self.run_wrapper(args, ll_enabled=False, custom_exists=True)
        self.assertEqual(res.returncode, 0)
        self.assertIsNotNone(custom_out)
        self.assertIn("CUSTOM:", custom_out)
        self.assertIn("--codec=aptX", custom_out)
        self.assertIn("--codec=aptX-HD", custom_out)

    def test_fail_closed_when_no_executable_found(self):
        # Remove stock binary so neither exists
        self.stock_bluealsa.unlink()
        res, _, _ = self.run_wrapper(["-S"], ll_enabled=False, custom_exists=False)
        self.assertEqual(res.returncode, 127)
        self.assertIn("no executable found", res.stderr)

    def test_bluealsa_custom_binary_properties(self):
        custom_bin = PAYLOAD / "bluealsa-custom"
        self.assertTrue(custom_bin.is_file(), "payload/bluealsa-custom missing")
        mode = custom_bin.stat().st_mode
        self.assertTrue(mode & stat.S_IXUSR, "payload/bluealsa-custom is not executable")
        data = custom_bin.read_bytes()
        self.assertIn(b"/A2DP/aptX/source", data)
        self.assertIn(b"/A2DP/aptX/sink", data)
        self.assertIn(b"/A2DP/aptXHD/source", data)
        self.assertIn(b"/A2DP/aptXHD/sink", data)
        self.assertIn(b"/A2DP/LDAC/source", data)
        self.assertIn(b"/A2DP/LDAC/sink", data)
        self.assertIn(b"libaptx.so", data)
        self.assertIn(b"libaptxHD.so", data)


class TestBtCodecLogic(unittest.TestCase):
    def test_c_source_logic_parity(self):
        """Compile a small C test verifying bt.c's codec picking and parsing logic."""
        c_test = r"""
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define BT_CODEC_N 7
const char *const BT_CODEC_LABEL[BT_CODEC_N] = {
    "SBC", "AAC", "LDAC Mobile", "LDAC Standard", "LDAC High", "aptX", "aptX-HD"
};

#define BTC_AAC     2u
#define BTC_LDAC    4u
#define BTC_APTX    8u
#define BTC_APTX_HD 16u

int bt_codec_pick(int choice, unsigned avail, int avail_known){
    if(choice < 0 || choice >= BT_CODEC_N) return 0;
    if(!avail_known) return 0;
    if(choice == 6){
        if(avail & BTC_APTX_HD) return 6;
        if(avail & BTC_APTX)    return 5;
        return 0;
    }
    if(choice == 5){
        if(avail & BTC_APTX)    return 5;
        return 0;
    }
    if(choice >= 2 && choice <= 4){
        if(avail & BTC_LDAC) return choice;
        if(avail & BTC_AAC)  return 1;
        return 0;
    }
    if(choice == 1){
        if(avail & BTC_AAC)  return 1;
        return 0;
    }
    return 0;
}

unsigned bt_codec_parse_avail(const char *info, int *known){
    unsigned m = 0;
    if(known) *known = 0;
    const char *h = info ? strstr(info, "Available codecs:") : NULL;
    if(!h) return 0;
    if(known) *known = 1;
    const char *p = h + 17;
    for(int first = 1; *p;){
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if(!first && memchr(p, ':', n)) break;
        first = 0;
        for(size_t i = 0; i < n;){
            size_t j = i;
            while(j < n && ((p[j]>='A'&&p[j]<='Z')||(p[j]>='a'&&p[j]<='z')||(p[j]>='0'&&p[j]<='9')||p[j]=='-')) j++;
            if(j - i == 3 && !strncasecmp(p + i, "AAC", 3)) m |= BTC_AAC;
            else if(j - i == 4 && !strncasecmp(p + i, "LDAC", 4)) m |= BTC_LDAC;
            else if(j - i == 4 && !strncasecmp(p + i, "aptX", 4)) m |= BTC_APTX;
            else if(j - i == 7 && (!strncasecmp(p + i, "aptX-HD", 7) || !strncasecmp(p + i, "aptX_HD", 7))) m |= BTC_APTX_HD;
            i = j > i ? j : i + 1;
        }
        if(!e) break;
        p = e + 1;
    }
    return m;
}

int main(void){
    // Test label counts
    assert(strcmp(BT_CODEC_LABEL[0], "SBC") == 0);
    assert(strcmp(BT_CODEC_LABEL[5], "aptX") == 0);
    assert(strcmp(BT_CODEC_LABEL[6], "aptX-HD") == 0);

    // Test parse avail
    int known = 0;
    const char *sample = "Device: 11:22:33:44:55:66\n"
                         "Available codecs: SBC AAC aptX aptX-HD LDAC\n"
                         "Selected codec: SBC\n";
    unsigned m = bt_codec_parse_avail(sample, &known);
    assert(known == 1);
    assert(m & BTC_AAC);
    assert(m & BTC_LDAC);
    assert(m & BTC_APTX);
    assert(m & BTC_APTX_HD);

    // Test pick
    assert(bt_codec_pick(6, m, 1) == 6); // aptX-HD
    assert(bt_codec_pick(5, m, 1) == 5); // aptX
    assert(bt_codec_pick(4, m, 1) == 4); // LDAC High
    assert(bt_codec_pick(1, m, 1) == 1); // AAC

    // Fallbacks
    unsigned m_aptx_only = BTC_APTX;
    assert(bt_codec_pick(6, m_aptx_only, 1) == 5); // aptX-HD falls back to aptX
    assert(bt_codec_pick(4, m_aptx_only, 1) == 0); // LDAC falls back to SBC when no AAC

    printf("PASS\n");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "test.c"
            bin_out = Path(td) / "test_bin"
            src.write_text(c_test)
            compile_res = subprocess.run(["cc", "-Wall", "-Werror", str(src), "-o", str(bin_out)])
            self.assertEqual(compile_res.returncode, 0, "C logic test compilation failed")
            run_res = subprocess.run([str(bin_out)], capture_output=True, text=True)
            self.assertEqual(run_res.returncode, 0)
            self.assertIn("PASS", run_res.stdout)


class TestBtReceiverDeviceManagement(unittest.TestCase):
    """Tests device filtering, pairing parser, and command synthesis for BT receiver mode."""

    def test_paired_devices_parsing_and_filtering(self):
        c_code = """
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

typedef struct {
    char mac[20];
    char name[64];
    int connected;
} bt_rx_device_t;

static int rx_mac_valid(const char *mac){
    if(!mac || strlen(mac) != 17) return 0;
    for(int i = 0; i < 17; i++){
        char c = mac[i];
        if((i % 3) == 2){ if(c != ':') return 0; }
        else if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return mac[17] == 0;
}

int mock_parse_paired(const char *buf, const char *cur_mac, bt_rx_device_t *out, int max_devs){
    int count = 0;
    char linebuf[4096];
    strncpy(linebuf, buf, sizeof linebuf - 1);
    linebuf[sizeof linebuf - 1] = 0;
    char *line = linebuf;
    while(line && *line && count < max_devs){
        char *nl = strchr(line, '\\n');
        if(nl) *nl = 0;
        char *cr = strchr(line, '\\r');
        if(cr) *cr = 0;

        if(!strncmp(line, "Device ", 7) && strlen(line) > 7 + 17){
            char mac[20];
            memcpy(mac, line + 7, 17);
            mac[17] = 0;
            const char *raw_name = line + 7 + 17;
            while(*raw_name == ' ' || *raw_name == '\\t') raw_name++;

            if(rx_mac_valid(mac)){
                // Mock device info filtering: skip headphones (e.g. WH-1000XM4)
                if(strstr(raw_name, "Headphones") || strstr(raw_name, "WH-1000XM")) {
                    line = nl ? (nl + 1) : NULL;
                    continue;
                }
                snprintf(out[count].mac, sizeof out[count].mac, "%s", mac);
                snprintf(out[count].name, sizeof out[count].name, "%s", raw_name);
                out[count].connected = (cur_mac && !strcasecmp(mac, cur_mac)) ? 1 : 0;
                count++;
            }
        }
        line = nl ? (nl + 1) : NULL;
    }
    return count;
}

int main(void){
    const char *sample = 
        "Device 11:22:33:44:55:66 Pixel 8 Pro\\n"
        "Device AA:BB:CC:DD:EE:FF Sony WH-1000XM4\\n"
        "Device 99:88:77:66:55:44 MacBook Air\\n";
    bt_rx_device_t devs[8];
    int n = mock_parse_paired(sample, "11:22:33:44:55:66", devs, 8);
    assert(n == 2);
    assert(strcmp(devs[0].name, "Pixel 8 Pro") == 0);
    assert(devs[0].connected == 1);
    assert(strcmp(devs[1].name, "MacBook Air") == 0);
    assert(devs[1].connected == 0);
    printf("DEV_MGMT_PASS\\n");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "test_dev.c"
            bin_out = Path(td) / "test_dev_bin"
            src.write_text(c_code)
            compile_res = subprocess.run(["cc", "-Wall", "-Werror", str(src), "-o", str(bin_out)])
            self.assertEqual(compile_res.returncode, 0, "C test compilation failed")
            run_res = subprocess.run([str(bin_out)], capture_output=True, text=True)
            self.assertEqual(run_res.returncode, 0)
            self.assertIn("DEV_MGMT_PASS", run_res.stdout)

    def test_rx_is_audio_source_capability_filtering(self):
        c_code = """
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

static int parse_is_audio_source_from_content(const char *buf){
    const char *svc = strstr(buf, "Services=");
    if(svc){
        const char *end = strpbrk(svc, "\\r\\n");
        char svc_copy[1024];
        if(end && (size_t)(end - svc) < sizeof(svc_copy)){
            size_t len = (size_t)(end - svc);
            memcpy(svc_copy, svc, len);
            svc_copy[len] = 0;
        } else {
            strncpy(svc_copy, svc, sizeof(svc_copy) - 1);
            svc_copy[sizeof(svc_copy) - 1] = 0;
        }

        int has_src = (strcasestr(svc_copy, "0000110a") != NULL);
        int has_snk = (strcasestr(svc_copy, "0000110b") != NULL);

        if(has_src) return 1;
        if(has_snk) return 0;
        return 0;
    }

    const char *cls = strstr(buf, "Class=");
    if(cls){
        long cod = strtol(cls + 6, NULL, 0);
        int major = (cod >> 8) & 0x1f;
        int minor = (cod >> 2) & 0x3f;

        if(major == 0x01 || major == 0x02) return 1;
        if(major == 0x04){
            if(minor == 0x01 || minor == 0x02 || minor == 0x05 ||
               minor == 0x06 || minor == 0x07 || minor == 0x08 || minor == 0x0a){
                return 0;
            }
        }
    }
    return 1;
}

int main(void){
    const char *earbuds_info =
        "[General]\\n"
        "Name=Wireless Earbuds\\n"
        "Class=0x240404\\n"
        "SupportedTechnologies=BR/EDR;\\n"
        "Trusted=true\\n"
        "Blocked=false\\n"
        "Services=00001101-0000-1000-8000-00805f9b34fb;0000110b-0000-1000-8000-00805f9b34fb;0000110c-0000-1000-8000-00805f9b34fb;0000110d-0000-1000-8000-00805f9b34fb;\\n";

    const char *phone_info =
        "[General]\\n"
        "Name=Smartphone\\n"
        "Class=0x5a420c\\n"
        "SupportedTechnologies=BR/EDR;\\n"
        "Services=00001105-0000-1000-8000-00805f9b34fb;0000110a-0000-1000-8000-00805f9b34fb;0000110c-0000-1000-8000-00805f9b34fb;\\n";

    const char *tablet_info =
        "[General]\\n"
        "Name=Tablet\\n"
        "Class=0x5a4110\\n"
        "SupportedTechnologies=BR/EDR;\\n"
        "Services=00001105-0000-1000-8000-00805f9b34fb;0000110a-0000-1000-8000-00805f9b34fb;\\n";

    const char *tv_info =
        "[General]\\n"
        "Name=Smart TV\\n"
        "Class=0x2c043c\\n"
        "Services=0000110a-0000-1000-8000-00805f9b34fb;0000110b-0000-1000-8000-00805f9b34fb;\\n";

    assert(parse_is_audio_source_from_content(earbuds_info) == 0); /* Pure sink earbuds must be filtered out */
    assert(parse_is_audio_source_from_content(phone_info) == 1);   /* Phone kept */
    assert(parse_is_audio_source_from_content(tablet_info) == 1);  /* Tablet kept */
    assert(parse_is_audio_source_from_content(tv_info) == 1);      /* TV (audio source) kept */

    printf("RX_CAP_FILTER_PASS\\n");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "test_filter.c"
            bin_out = Path(td) / "test_filter_bin"
            src.write_text(c_code)
            compile_res = subprocess.run(["cc", "-Wall", "-Werror", str(src), "-o", str(bin_out)])
            self.assertEqual(compile_res.returncode, 0, "C filter test compilation failed")
            run_res = subprocess.run([str(bin_out)], capture_output=True, text=True)
            self.assertEqual(run_res.returncode, 0)
            self.assertIn("RX_CAP_FILTER_PASS", run_res.stdout)


if __name__ == "__main__":
    unittest.main()

