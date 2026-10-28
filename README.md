# LegionBtFix

A Lilu plugin that patches two instructions of `/usr/sbin/bluetoothd` in memory on macOS Tahoe (tested on 26.7.1 25G241; the patterns also match 26.7.2 25G332), for two things the MediaTek MT7925 Bluetooth controller does that `bluetoothd` does not expect. Nothing on disk changes. It only works on those exact builds: on any other `bluetoothd` the page hashes do not match and the plugin does nothing.

1. **`bluetoothd` crash loop on LE connections ("STATUS 718").** `bluetoothd` sends the legacy LE Create Connection (HCI 0x200D) with a random own address before it has ever set one. The MT7925 rejects that with 0x12 (Invalid HCI Command Parameters), as the Core spec says it should when no random address is set. `bluetoothd` treats the error as fatal, aborts, launchd restarts it, and the loop repeats every few seconds. The patch makes it always use the public address.
2. **AirPods silent over A2DP ("Failed to read BT clock").** Before every AVDTP START, `bluetoothd` sends HCI Read Clock and the MT7925 answers with Connection_Handle 0x0000 instead of the handle it was asked about. `bluetoothd` finds no link for handle 0, logs the error and never stores the clock to host-time mapping the time-synced AAC-ELD stream needs, so the AirPods play nothing. The patch skips that link check; the clock values in the reply are correct, only the handle is wrong.

The controller itself is brought up by [MtkBt](https://github.com/watchthelight-labs/MtkBt), which loads its firmware. You need MtkBt, BlueToolFixup and this plugin together.

## Status

Legion 5 15IAX10, macOS 26.7.1 (25G241), OpenCore 1.0.8, Lilu 1.7.2, BlueToolFixup 2.7.2, MtkBt 0.2.0 (0.2.4 is loaded there now; the runs below were on 0.2.0), AirPods Pro:

| Patch | Result |
|---|---|
| LE own address | no `bluetoothd` crash reports since the patch went in (2026-10-17); before it, a crash every few seconds while any LE device was known |
| BT clock | `Failed to read BT clock` is gone from the log, and the A2DP stream's RTP timestamps are anchored (checked in the packet logger) |
| AirPods audio | **plays, but only with a workaround** (below). Without it the stream starts and the link drops about 10 s after the first media packet |

Also confirmed on an ASRock X870 Steel Legend WiFi (AMD RZ717 card, Bluetooth `0E8D:0717`, macOS 26.7.1 25G241, MtkBt 0.2.2), 2026-10-18: before the plugin, 13 `bluetoothd` crash reports in under three minutes and Bluetooth turning itself off seconds after it came up; with it, devices pair and audio plays. The page hashes depend only on the `bluetoothd` build, so any 25G241 Mac with an MT7925 gets the same fix.

This plugin does not fix the remaining problem. With both patches in, music is heard in 3 of 3 runs when `bluetoothd`'s own HCI packet logging with raw audio is on, and in 0 of 3 runs when it is off. In the failing runs the controller stops reporting Number Of Completed Packets right after the first media packet, nothing comes back from the AirPods, 8 ACL packets (the controller's whole buffer) stay outstanding, and the link ends with Disconnection Complete reason 0x08 (connection timeout) about 10 s later. LE reports and HCI commands keep working meanwhile. The logging flag only changes how many payload bytes the packet logger copies, so the effect is timing or chance. The open lead is Wi-Fi and Bluetooth coexistence on the shared MT7925 radio (the Wi-Fi driver, MtkWlan, sends no coexistence information to the firmware).

The workaround, which survives reboots and is applied by `bluetoothd` at start:

```
sudo tools/bt-hci-logging.sh on      # writes HCITraces to /Library/Preferences/com.apple.MobileBluetooth.debug and restarts bluetoothd
sudo tools/bt-hci-logging.sh off     # removes it
tools/bt-hci-logging.sh status
```

It makes `bluetoothd` write HCI traces with audio payloads to `/var/root/Library/Logs/Bluetooth/bluetoothd-hci-*.pklg` (three files of about 4 MB, rotated). Restarting `bluetoothd` while the AirPods are connected makes them reject the next AVDTP START with SEP_IN_USE until they reconnect (case in and out).

## What it can't do

- It finds each patch site by a byte window around it, with the parts that change between builds masked, so it does not need a list of builds. A Tahoe update that changes the code around either site makes that patch silently stop applying; the window then needs re-taking (`docs/HOW-IT-WORKS.md` says how).
- Side effects: LE connections from this Mac no longer use a random (private) address; `bluetoothd` stores the local clock for an A2DP stream even if the link it asked about is gone (it is the piconet clock while the Mac is central, which it is for headphones).
- It does not fix the link drop described above.
- Tested on one laptop with one controller.

## Installation

1. Get `LegionBtFix.kext` from the [releases](../../releases), or build it (below).
2. Add it to `config.plist`, `Kernel > Add`, after Lilu and after BlueToolFixup (both patch `bluetoothd`; the two windows overlap none of BlueToolFixup's patterns, so the order only matters for Lilu):

   ```
   BundlePath      LegionBtFix.kext
   ExecutablePath  Contents/MacOS/LegionBtFix
   PlistPath       Contents/Info.plist
   MinKernel       25.0.0
   MaxKernel       25.99.99
   Enabled         true
   ```

3. Reboot and check:

   ```
   log show --last 10m --predicate 'eventMessage CONTAINS "lbtfx"'
   ```

   should show `LE own address forced to public` and, once audio starts, `BT clock accepted without link lookup`. No line at all on a new build means the code around the sites changed: no patch was applied. Then:

   ```
   ls /Library/Logs/DiagnosticReports | grep bluetoothd        # no new crash reports
   log show --last 10m --predicate 'process == "bluetoothd" AND eventMessage CONTAINS "BT clock"'
   ```

4. Rollback: set `Enabled` to false, or boot with `-legionbtfixoff`.

## Boot-args

| Boot-arg | Effect |
|---|---|
| `-legionbtfixoff` | plugin disabled |
| `-legionbtfixnoaddr` | skip the LE own address patch |
| `-legionbtfixnoclock` | skip the BT clock patch (for an A/B test) |
| `-legionbtfixdbg` | debug logging, DEBUG build only |

## Building

```
tools/build-mac.sh                    # clones acidanthera/Lilu at 1.7.2 into ./Lilu on first run; out/LegionBtFix.kext
LILU_KEXT=/path/to/Lilu.kext tools/build-mac.sh   # adds a check that every Lilu symbol the plugin needs is exported
```

Needs the Xcode Command Line Tools. The build is reproducible: the 0.2.0 release binary (sha256 `3c330a217c47d398...`, full value in the release notes) is the file running on the test laptop, and a clean build of this tree with Apple clang 17 gives the same bytes.

`tools/` also holds three small parsers for `bluetoothd`'s `.pklg` packet logs (`pklg-summary.py`, `pklg-stream-timing.py`, `pklg-rtp-starts.py`), which is how the link drop above was measured.

## How it works

Like BlueToolFixup, the plugin routes the kernel's `_cs_validate_page` and looks at every page of `/usr/sbin/bluetoothd` as it is loaded. A page is patched only if the kernel is Tahoe, the file path matches, and the page holds the masked byte window around the patch site. The two patches are `movzx ecx, al` to `mov cl, 1; nop` (always the public address) and `je` to `jmp` (skip the link check). The disassembly, the HCI evidence and how to re-take the windows for another build: [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md).

## Credits

- **BlueToolFixup** (acidanthera/BrcmPatchRAM, created by Dhinak G, GPL-2.0): the `_cs_validate_page` route, the path check and the plugin shape are adapted from it.
- **Lilu** (acidanthera, vit9696, BSD-3-Clause): the patching framework and `plugin_start.cpp`.
- **MtkBt** and the Linux btmtk driver for the controller side.

## Licence

GPL-2.0, because the page-patching code is adapted from BlueToolFixup. `Lilu/` (cloned at build time) is BSD-3-Clause.
