# Changelog

## 0.3.0 (2026-10-28)

- macOS 26.7.2 (25G332). Both patches now also match 25G332's `bluetoothd`, with that build's page hashes. The address window is byte-for-byte the same (it moved 0x1b0 bytes within its page). The clock window has new call and log-handle displacements. 25G241 stays supported. Taken from `bluetoothd` in Apple's full 25G332 OTA and checked page by page against the plugin's own matching logic. Not yet booted on 25G332.
- Log lines name the build that matched (`bluetoothd 25G332: ...`).

## 0.2.0 (2026-10-17)

- Second patch: the A2DP Read Clock completion stores the clock even though the MT7925 returns handle 0. `Failed to read BT clock` is gone and the stream's RTP timestamps are anchored.
- Boot-args `-legionbtfixnoaddr` and `-legionbtfixnoclock` to disable one patch each.
- `tools/bt-hci-logging.sh` for the link-drop workaround, and the `.pklg` parsers used to measure it.

## 0.1.0 (2026-10-17)

- First patch: LE Create Connection always uses the public own address, which ends the `bluetoothd` crash loop (STATUS 718) on the MT7925 with macOS 26.7.1 (25G241).
