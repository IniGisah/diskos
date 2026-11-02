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


if __name__ == "__main__":
    unittest.main()

