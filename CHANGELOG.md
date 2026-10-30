# Changelog

## Unreleased

- Opt-in boot-arg `-legionbtfixlewait` adds two patches for BLE devices that stall after the link comes up: `bluetoothd` parks a new LE connection (probe states `LE_READY_WAIT_FOR_PHY`, `LE_READY_WAIT_FOR_LENGTH`) until the controller sends the LE PHY Update and Data Length Change events, and the MT7925 may never send them. Each of the two `je`/`jne` jumps that pick a wait state becomes a never-taken `jo` (2 bytes, `0f 84` or `0f 85` to `0f 80`). Both masked windows match exactly once in 25G332's `bluetoothd`. Built, found statically, not booted. Off unless the boot-arg is set.

## 0.3.0 (2026-10-28)

- No longer tied to one macOS build. Each patch is now a byte window with the parts that change between builds masked (call displacements, a stack slot, the `je` distance, the log handle), searched anywhere in each `bluetoothd` page, instead of fixed offsets plus a SHA-1 of the page. The windows match exactly once in both 26.7.1 (25G241) and 26.7.2 (25G332) `bluetoothd` and nowhere else in either file. A later Tahoe build where the code around either site changed gets no patch, as before.
- Log lines give the page and the offset in it (`bluetoothd: ... (page 6d3000 +af2)`).
- Not yet booted on 25G332.

## 0.2.0 (2026-10-17)

- Second patch: the A2DP Read Clock completion stores the clock even though the MT7925 returns handle 0. `Failed to read BT clock` is gone and the stream's RTP timestamps are anchored.
- Boot-args `-legionbtfixnoaddr` and `-legionbtfixnoclock` to disable one patch each.
- `tools/bt-hci-logging.sh` for the link-drop workaround, and the `.pklg` parsers used to measure it.

## 0.1.0 (2026-10-17)

- First patch: LE Create Connection always uses the public own address, which ends the `bluetoothd` crash loop (STATUS 718) on the MT7925 with macOS 26.7.1 (25G241).
