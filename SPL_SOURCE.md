# Stage-1 SPL - corresponding source & build (GPL-2.0)

The installer's USB stage-1 loader (`flash/disc_spl_lpddr3.bin`) that brings up DRAM before
the on-device NAND writer runs is **built from GPL source** - no vendor/USBCloner binary is
redistributed - **plus a small DDR parameter block (about 332 bytes of board information and DRAM register
settings) captured from the device's factory configuration**; see "DDR parameter block" below
for its vendor-origin provenance. This file is the GPL-2.0 "corresponding source" pointer + build recipe.

## Source

- **Upstream:** Ingenic-community **uboot-xburst**, U-Boot 2013.07, commit `1060b516` (GPL-2.0,
  `COPYING` in-tree). https://github.com/Ingenic-community/uboot-xburst
- **Our patches** (a small, self-contained delta adding W63AH6NKB LPDDR3 support to the X2000
  USB "burner" SPL, ported from the GPL tobunto/u-boot X2000 tree):
  1. `-fcommon` + a mask-ROM return shim in `arch/mips/cpu/xburst2/x2000/start.S` (DDR-only
     stage-1 returns to the mask ROM after DRAM init).
  2. `ddr_mr11` field + macro in `ddr_reg_data.h`.
  3. `case LPDDR3:` MR sequence (MR63,10,1,2,3,11) in `ddrc_dfi_init()`.
  4. Force `type = LPDDR3` in `sdram_init()` (this part's controller CFG.TYPE reads as LPDDR2).
  5. **PHY PLL divisors `FBDIV=8, PDIV=4`** for W63AH6NKB LPDDR3 @400 MHz (the tree's stock
     20/5 sets the wrong PHY clock and DDR training never converges).
  6. Bounded DDR initialization polls + a small TCSM breadcrumb record for
     field diagnostics.
  7. **Robust watchdog disable** (clear `WDT_TCER.TCEN` then stop the WDT clock via `TCU_TSSR`)
     **and omit the dynamic-calibration sweep**. The counter is disabled before its clock
     is stopped; the burner uses the single calibration at the final 400 MHz rate.

The complete patched source is shipped as `spl-src/uboot-xburst-lpddr3-src.tar.gz`, with the
delta over upstream also provided as a readable patch series in `spl-src/patches/`. Frozen at the
commit the released binary was built from: `7caf048` / tag `lpddr3-working-v2` (Ingenic-community/uboot-xburst `1060b516` + 8 patches).

## Toolchain

- `gcc-10-mipsel-linux-gnu` (10.5.0) + binutils for mipsel. (The tree's compiler-compat headers
  stop at gcc-10; newer GCC fails. Pin GCC 10.)

## Build + assemble

```sh
export ARCH=mips CROSS_COMPILE=mipsel-linux-gnu-      # gcc-10 on PATH as mipsel-linux-gnu-gcc
make distclean
make burner_x2000_lpddr2_config
make -j                                               # -> spl/u-boot-spl.bin
# USB stage-1 = DDR param block + zero-pad to 0x800 + spl/u-boot-spl.bin
#   load 0xb2401000, exec 0xb2401800; see flash/assemble_stage1.py
```

## DDR parameter block

The stage-1 prepends a 0x800-byte parameter region the SPL reads at `0xb2401000`:
a **BDIF** (board-info) record + a **DDR** record of 35 Winbond W63AH6NKB-BI register words
(controller/PHY/MR/timing values - hardware register settings for LPDDR3 @400 MHz, 24 MHz
EXTAL, 800 MHz PLL).

**Provenance:** these values were **captured from the device's factory
configuration** (they match what the vendor SPL uses), NOT independently re-derived from the
W63AH6NKB datasheet. `flash/assemble_stage1.py` copies roughly **332 bytes** of this captured
block into the shipped stage-1 binary. The provenance of these vendor-origin bytes
is an **unresolved compliance item**. Before any wide/public
binary distribution it should be resolved by one of: (a) independently regenerating the complete
parameter/BDIF block from documented datasheet values; (b) obtaining and documenting redistribution
permission; or (c) having counsel review and approve the provenance with an accurate disclosure of
the copied vendor-origin bytes. Until then the captured block ships as the working block and reference input.

## Supported hardware

This stage-1 is built for the **Winbond W63AH6NKB-BI LPDDR3** DRAM and configures that chip.

The Disc configuration uses `LPDDR3_W63AH6NKB-BI`. This loader does not
select between DRAM chips; a revision using different DRAM would need a
matching loader.

Bounded DDR polls record a failure at `0xb24017c0` and stop waiting on
timeout. They continue the initialization sequence; they do not abort the
flash or establish that DRAM is usable. The SPL returns to the mask ROM
without writing NAND. Recovery is through mask-ROM mode.

The installer does **not** run a separate DRAM round-trip before erasing.
Marginal DRAM can corrupt the image during programming; NAND verification
against that same DRAM may not detect the corruption.

## Verification

The assembly script checks the BDIF magic, DDR payload length, diagnostic
scratch boundary and TCSM size limit. The writer checks the downloaded image's
SHA256 and the Disc's firmware base before unlocking or erasing NAND. These
checks do not replace a DRAM round-trip.
