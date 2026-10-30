// <wtl> project: legionbtfix | file: src/LegionBtFix.cpp | author: watchthelight </wtl>
// LegionBtFix: a Lilu plugin that patches single instructions of macOS Tahoe /usr/sbin/bluetoothd in
// memory, as its pages are loaded, to work around two MT7925 controller behaviours bluetoothd does not expect.
//
// Patch 1, LE own address. Why (the HCI trace in docs/HOW-IT-WORKS.md): bluetoothd sends the legacy LE
// Create Connection (HCI 0x200D) with Own_Address_Type 1 (random) 55 s before it ever sends LE Set Random Address
// (0x2005). The MT7925 answers with Command Status 0x12 (Invalid HCI Command Parameters), as the Core spec requires
// when no random address is set. bluetoothd maps that to "STATUS 718", treats it as fatal in its LE Connection
// Complete handler and aborts, launchd restarts it, and the loop repeats. The same command with Own_Address_Type 0
// (public) is accepted every time.
// In the LE connection manager (25G241 x86_64 slice, function at 0x1006cf0fe), the own-address choice comes from the
// helper at 0x1006e9fd8 ("Using public address because device ... isn't a cloud pair device" / "Still using random
// address to connect"), whose bool (1 = public) is passed to the Stack.LE auto-connect at 0x10021a5d2:
//     1006cfca5  call   0x1006e9fd8
//     1006cfcaa  mov    ecx, 0xff ; mov [rsp+8], ecx ; mov [rsp], ecx
//     1006cfcb6  movzx  ecx, al          0f b6 c8   ->   mov cl, 1 ; nop   b1 01 90
// ecx holds 0xff before, so after "mov cl, 1" it is exactly 1: always public. Nothing else changes.
//
// Patch 2, BT clock (AirPods A2DP silent, docs/HOW-IT-WORKS.md). Before every AVDTP START, bluetoothd
// sends HCI Read Clock (0x1407) for the link with Which_Clock 0 (local). The MT7925 answers with status 0 and a valid
// clock but Connection_Handle 0x0000 instead of the handle that was asked for. The stack's reply parser (readClockCB,
// 0x10017d2ad) looks the link up by that handle (0x10017fb40) and finds none, so the A2DP completion block
// (0x100457c09) gets 0x515 from the link check (0x10015f8b0), logs "Failed to read BT clock" and skips the call that
// stores the clock to host-time mapping (0x10044fe4e). The time-synced AAC-ELD stream then has no anchor and the
// AirPods play nothing. The mapping function only takes status, clock, accuracy and host time (not the link), and
// the local clock is the piconet clock while this Mac is central, so the link check is skipped:
//     100457c4d  test   r14d, r14d
//     100457c50  je     0x100457c77          74 25      ->   jmp 0x100457c77   eb 25
// A non-zero HCI status is still handled inside 0x10044fe4e, which logs it and stores nothing.
//
// Gating. Each patch is applied only where all of these hold:
//   * the kernel is Tahoe 25.x (Lilu min/max kernel) and the page belongs to /usr/sbin/bluetoothd (vn_getpath);
//   * the page holds the patch's 43 or 51 byte window, with only the call displacements, the stack slot, the je
//     distance and the RIP-relative log handle masked (they change with every build). The patched bytes may be the
//     original or the patched form, so an already patched page is recognised and left alone.
// The addresses above are from 25G241. The same masked windows match exactly once in 25G241's and 25G332's bluetoothd
// and nowhere else in either file, so the plugin carries no per-build table.
// A build whose code around either site changed matches nothing, and the plugin does nothing. Boot-arg
// -legionbtfixoff disables it entirely, -legionbtfixnoaddr and -legionbtfixnoclock disable one patch each.
// Patches 3 and 4 (LE ready gate, below) are opt-in: they apply only with -legionbtfixlewait.
//
// Adapted from BlueToolFixup.cpp (BrcmPatchRAM/BlueToolFixup.cpp, created by
// Dhinak G, part of acidanthera/BrcmPatchRAM, GNU General Public License v2): the _cs_validate_page route, the
// vn_getpath check of the page's file and the Lilu plugin configuration. Because of that this file, and the built
// kext, are under the GPL v2 as well (see README.md). Lilu headers and Library/plugin_start.cpp are from
// acidanthera/Lilu 1.7.2 (BSD 3-clause, Copyright (c) 2016-2018 vit9696).

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>

#define MODULE_SHORT "lbtfx"

namespace {
constexpr char kPath[] = "/usr/sbin/bluetoothd";
constexpr size_t kPage = 4096;
constexpr uint8_t _ = 0;   // wildcard in a mask

// Patch 1, LE own address. Call target and stack slot vary between builds and are masked; the rest is matched exactly.
const uint8_t kAddrWindow[43] = {
    0x48, 0x89, 0xdf,                         // mov    rdi, rbx
    0xe8, 0x2e, 0xa3, 0x01, 0x00,             // call   <own-address choice>
    0xb9, 0xff, 0x00, 0x00, 0x00,             // mov    ecx, 0xff
    0x89, 0x4c, 0x24, 0x08,                   // mov    [rsp+8], ecx
    0x89, 0x0c, 0x24,                         // mov    [rsp], ecx
    0x0f, 0xb6, 0xc8,                         // movzx  ecx, al      <- patched
    0x45, 0x0f, 0xb7, 0xc6,                   // movzx  r8d, r14w
    0x45, 0x0f, 0xb7, 0xcd,                   // movzx  r9d, r13w
    0x44, 0x89, 0xff,                         // mov    edi, r15d
    0x44, 0x89, 0xe6,                         // mov    esi, r12d
    0x8b, 0x95, 0xe4, 0xfe, 0xff, 0xff,       // mov    edx, [rbp-0x11c] (initPHYs)
};
const uint8_t kAddrMask[43] = {
    1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, _, _, _, _,
};
const uint8_t kAddrPatched[3] = {0xb1, 0x01, 0x90};   // mov cl, 1 ; nop

// Patch 2, BT clock. The three calls, the je distance and the log handle vary between builds and are masked.
const uint8_t kClockWindow[51] = {
    0x48, 0x8d, 0x7d, 0xef,                   // lea    rdi, [rbp-0x11]
    0xc6, 0x07, 0x00,                         // mov    byte [rdi], 0
    0xe8, 0x05, 0x43, 0xe6, 0xff,             // call   <lock>
    0x48, 0x8b, 0x7b, 0x20,                   // mov    rdi, [rbx+0x20] (link from the reply handle)
    0x4c, 0x89, 0xf6,                         // mov    rsi, r14
    0xe8, 0x6f, 0x7c, 0xd0, 0xff,             // call   <link check, 0x515 for no link>
    0x41, 0x89, 0xc6,                         // mov    r14d, eax
    0x48, 0x8d, 0x7d, 0xef,                   // lea    rdi, [rbp-0x11]
    0xe8, 0xfb, 0x42, 0xe6, 0xff,             // call   <unlock>
    0x45, 0x85, 0xf6,                         // test   r14d, r14d
    0x74, 0x25,                               // je     <store the clock>  <- opcode patched
    0x48, 0x8d, 0x05, 0xff, 0xac, 0x6f, 0x00, // lea    rax, [log handle]
    0x48, 0x8b, 0x18,                         // mov    rbx, [rax]
};
const uint8_t kClockMask[51] = {
    1, 1, 1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1,
    1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, _, 1, 1, 1, _, _, _, _, 1, 1, 1,
};
const uint8_t kClockPatched[1] = {0xeb};   // jmp short, same distance

// Patch 3 and 4 (opt-in, -legionbtfixlewait): bluetoothd's LE connection "ready" gate (probe state LE_READY_WAIT_FOR_PHY /
// LE_READY_WAIT_FOR_LENGTH, 25G332 function at 0x100209f45). After a link comes up it parks the connection until an LE PHY
// Update and an LE Data Length Change event have arrived and only then sends its first message. A controller that never sends
// those events leaves the connection parked (BLE HID pairing stalls). The function picks the wait state from two conditions:
//     100209f65  cmp   byte [rbx+0x99], 0      80 bb 99 00 00 00 00   phy event seen?
//     100209f6c  je    <state 4, wait-for-phy>  0f 84 a4 00 00 00
//     100209f7b  cmp   dword [rbx+0x88], 1      83 bb 88 00 00 00 01   data length probe complete?
//     100209f82  jne   <state 3, wait-for-length> 0f 85 9a 00 00 00
// Each jump becomes "jo" (0f 80, same length, OF is clear after test/cmp against a small value so it is never taken), which
// falls through to the code that runs once the events have arrived. Nothing else changes.
const uint8_t kPhyWindow[22] = {
    0x85, 0xd2, 0x74, 0x0d,                   // test   edx, edx ; je +0xd
    0x80, 0xbb, 0x99, 0x00, 0x00, 0x00, 0x00, // cmp    byte [rbx+0x99], 0
    0x0f, 0x84, 0xa4, 0x00, 0x00, 0x00,       // je     <wait-for-phy>  <- opcode patched
    0x40, 0x84, 0xf6,                         // test   sil, sil
    0x0f, 0x84,                               // je     <return>
};
const uint8_t kPhyMask[22] = {
    1, 1, 1, _, 1, 1, 1, 1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, 1,
};
const uint8_t kLenWindow[32] = {
    0x83, 0xbb, 0x88, 0x00, 0x00, 0x00, 0x01, // cmp    dword [rbx+0x88], 1
    0x0f, 0x85, 0x9a, 0x00, 0x00, 0x00,       // jne    <wait-for-length>  <- opcode patched
    0x80, 0x7b, 0x7d, 0x01,                   // cmp    byte [rbx+0x7d], 1
    0x0f, 0x85, 0xb3, 0x00, 0x00, 0x00,       // jne    <direct continue>
    0x80, 0xbb, 0x64, 0x01, 0x00, 0x00, 0x01, // cmp    byte [rbx+0x164], 1
    0x0f, 0x85,                               // jne    <direct continue>
};
const uint8_t kLenMask[32] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, 1, 1, _, _, _, _, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};
const uint8_t kJoPatched[2] = {0x0f, 0x80};   // jo rel32, never taken

struct Patch {
    const char *what;            // log text
    const uint8_t *window;
    const uint8_t *mask;
    size_t windowLen;
    size_t anchorOff;            // a run of exact bytes searched first, cheap for the pages that are not bluetoothd
    size_t anchorLen;
    size_t patchOff;             // patched bytes inside the window
    const uint8_t *patched;
    size_t patchedLen;
    bool enabled;
    bool done;
};

Patch patches[] = {
    {"LE own address forced to public", kAddrWindow, kAddrMask, sizeof(kAddrWindow), 8, 12, 20, kAddrPatched,
     sizeof(kAddrPatched), true, false},
    {"BT clock accepted without link lookup", kClockWindow, kClockMask, sizeof(kClockWindow), 24, 8, 39, kClockPatched,
     sizeof(kClockPatched), true, false},
    {"LE ready gate: no wait for PHY update", kPhyWindow, kPhyMask, sizeof(kPhyWindow), 4, 7, 11, kJoPatched,
     sizeof(kJoPatched), false, false},
    {"LE ready gate: no wait for data length", kLenWindow, kLenMask, sizeof(kLenWindow), 0, 7, 7, kJoPatched,
     sizeof(kJoPatched), false, false},
};

mach_vm_address_t orig_cs_validate {};

// The masked window, with the patched bytes either original or already patched.
bool windowMatches(const Patch &pt, const uint8_t *p, bool &alreadyPatched) {
    for (size_t i = 0; i < pt.windowLen; i++) {
        if (i >= pt.patchOff && i < pt.patchOff + pt.patchedLen)
            continue;
        if (pt.mask[i] && p[i] != pt.window[i])
            return false;
    }
    alreadyPatched = memcmp(p + pt.patchOff, pt.patched, pt.patchedLen) == 0;
    return alreadyPatched || memcmp(p + pt.patchOff, pt.window + pt.patchOff, pt.patchedLen) == 0;
}

// Offset of the window in the page, or -1. A window that crosses a page boundary is not found.
long findWindow(const Patch &pt, const uint8_t *page, bool &alreadyPatched) {
    const uint8_t *anchor = pt.window + pt.anchorOff;
    for (size_t at = pt.anchorOff; at + pt.windowLen - pt.anchorOff <= kPage; at++) {
        if (page[at] != anchor[0] || memcmp(page + at, anchor, pt.anchorLen) != 0)
            continue;
        if (windowMatches(pt, page + at - pt.anchorOff, alreadyPatched))
            return static_cast<long>(at - pt.anchorOff);
    }
    return -1;
}

// As BlueToolFixup's patched_cs_validate_page: let the kernel validate first, then patch the page contents.
void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data,
                              int *validated_p, int *tainted_p, int *nx_p) {
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    auto page = static_cast<uint8_t *>(const_cast<void *>(data));
    if (UNLIKELY(!page))
        return;
    bool pathChecked = false;
    for (auto &pt : patches) {
        bool already = false;
        long off = pt.enabled ? findWindow(pt, page, already) : -1;
        if (LIKELY(off < 0))
            continue;
        if (!pathChecked) {
            char path[PATH_MAX];
            int pathlen = PATH_MAX;
            if (vn_getpath(vp, path, &pathlen) != 0 || strcmp(path, kPath) != 0)
                return;
            pathChecked = true;
        }
        if (already) {
            DBGLOG(MODULE_SHORT, "bluetoothd page at %llx already patched", (unsigned long long)page_offset);
            continue;
        }
        memcpy(page + off + pt.patchOff, pt.patched, pt.patchedLen);
        if (!pt.done) {
            pt.done = true;
            SYSLOG(MODULE_SHORT, "bluetoothd: %s (page %llx +%lx)", pt.what, (unsigned long long)page_offset, off);
        }
    }
}

void pluginStart() {
    SYSLOG(MODULE_SHORT, "start");
    patches[0].enabled = !checkKernelArgument("-legionbtfixnoaddr");
    patches[1].enabled = !checkKernelArgument("-legionbtfixnoclock");
    // Opt-in: not part of the proven default set.
    patches[2].enabled = patches[3].enabled = checkKernelArgument("-legionbtfixlewait");
    if (patches[2].enabled)
        SYSLOG(MODULE_SHORT, "opt-in LE ready gate patches on (-legionbtfixlewait)");
    for (size_t i = 0; i < 2; i++)
        if (!patches[i].enabled)
            SYSLOG(MODULE_SHORT, "disabled by boot-arg: %s", patches[i].what);
    lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) {
        KernelPatcher::RouteRequest csRoute("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
            SYSLOG(MODULE_SHORT, "failed to route cs validation pages");
    });
}

const char *bootargOff[] {"-legionbtfixoff"};
const char *bootargDebug[] {"-legionbtfixdbg"};
const char *bootargBeta[] {"-legionbtfixbeta"};
} // namespace

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Tahoe,
    KernelVersion::Tahoe,
    pluginStart
};
