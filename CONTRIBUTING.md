# Contributing

Two kinds of report help most: whether the patches apply on your build (`log show --last 10m --predicate 'eventMessage CONTAINS "lbtfx"'`), and whether your AirPods or other A2DP device play without the HCI logging workaround. Open an issue with the laptop model, the macOS build, the controller (USB id), the versions of Lilu, BlueToolFixup, MtkBt and this plugin, and the `lbtfx` log lines. If you re-take the windows and hashes for a newer `bluetoothd`, a PR with the new values and the disassembly excerpts is very welcome.

Pull requests: keep them small and say how you tested them. I read and test every PR before merging. AI-assisted PRs are fine as long as you can explain the change, which is the same bar I hold my own code to.

This plugin is GPL-2.0 because its page-patching code comes from BlueToolFixup; contributions are under the same licence.
