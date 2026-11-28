# Offline service contracts

These providers implement explicit service lifecycles and availability errors
for the Android runtime. A resolved import is not proof that a
feature, menu or race works. The device must still execute the original game
with its own files, and the renderer must produce valid output.

## Dialogs

`runtime_services.c` serializes CommonDialog ownership. Initialize requires
CommonDialog initialization, a second owner receives `BUSY`, duplicate
initialization reports the documented error, and termination releases ownership.
The states are `NONE`, `INITIALIZED`, `RUNNING` and `FINISHED`.

| Service | Implemented behavior | Remaining limitation |
| --- | --- | --- |
| Message | Validates the 136-byte parameter, tracks open/close/status, returns the complete 44-byte result. A choice without UI finishes with cancellation and an invalid button. | Native message rendering and affirmative choices are absent. |
| SaveDataDialog | Validates the 152-byte parameter. Progress and passive operation notices remain running until the caller closes them. SetValue validates target/range and never completes a save by itself. Results preserve userData and caller-owned output pointers. | Save selection/confirmation UI is absent. Choice requests are canceled; no directory or savedata metadata is fabricated. The actual SaveData filesystem provider remains responsible for saving. |
| Profile | Returns the complete 48-byte result with signed-out error, cancellation and the original userData. Close/status/terminate use the same lifecycle. | No PSN profile UI or account action. |
| Commerce | Validates the 128-byte parameter and returns the complete 48-byte result, cancellation, `authorized=false` and the original userData. GetResult returns the commerce result code as well. | No purchase, license grant, PS Plus authorization or store access. |
| Error | Uses its own initialization and `0x80ED...` errors. Open validates the 16-byte parameter and writes the actual error code/user to diagnostics. Close or UpdateStatus finishes the logged notice. | The error is available in the persistent diagnostic log; no platform error dialog is drawn. |
| IME | Existing renderer text entry remains available where supported. If that backend declines, the result is canceled and the guest buffer remains unchanged. | Android has no native IME integration yet. Unavailable input no longer silently inserts or accepts `Player`. |

A progress result of `OK` after the caller's explicit Close only means the
notification was closed. Its button is invalid and it selects no save. It does
not independently certify that the game's file operation succeeded.

ABI checks use `runtime_dialogs.h`. Message result `mode` is reserved/fixed to
zero in the reference ABI, rather than echoing the requested dialog mode. Save
result `dirName` and `param` are caller-owned buffer pointers, not provider-owned
allocations. Canceled/progress-only results leave their contents untouched.

The provider also accepts legacy message button type 4, even though current
reference enums omit it. It follows the same cancellation lifecycle as other
choices when no dialog UI is available.

The GameCustomDataDialog and Facebook dialog imports remain absent. The
available reference declarations do not establish their result layouts; this
change does not fill those gaps with guessed structures or successful stubs.

## Voice, input and system policy

Voice ports are bounded, allocated logical objects with initialization,
deletion, shutdown and stored volume. The `CreatePort` argument order is
`(outPort, param)`.
`GetPortInfo` writes exactly 32 bytes and retains the caller's edge-buffer
pointer. The previous provider reversed CreatePort's arguments and cleared
40 bytes for PortInfo; both could corrupt guest memory.

Creating a device-type logical endpoint does not open a microphone. Port state
remains idle, output reads report zero frames without overwriting the audio
buffer, and unavailable routes/writes return a topology error. Writes report
zero consumed bytes. There is no voice codec, voice capture or online chat
implementation in this provider. SetVolume/GetVolume affect the logical port
and use the documented integer/float ABI.

AudioIn remains unopened. GetSilentState distinguishes malformed handles from
a correctly shaped but unopened handle; it never reports a working microphone.
Console background-music enable/disable changes the provider's policy only:
there is no console background player, and no other Android app's playback is
controlled. Android GameActivity already holds `FLAG_KEEP_SCREEN_ON` while
visible, which supplies the inactivity policy requested by PowerTick.

## Screenshot policy and recording availability

`runtime_capture.c` connects ScreenshotEnable/Disable to the Android game
window. Enable clears `WindowManager.LayoutParams.FLAG_SECURE`; Disable sets
it. The Android bridge applies the policy on the UI thread and confirms that
the live window accepted it. An unavailable hook/window, a failed application
or a retired hook returns an error instead of reporting an applied policy.
The runtime does not hold its capture mutex while waiting for the UI callback.

This controls whether Android may capture the game window. It does not create
a screenshot file or implement a console recorder. Android's `FLAG_SECURE`
policy also prevents the protected window from appearing on non-secure displays.
`sceVideoRecordingSetInfo`
validates the metadata kind and pointer, then returns `UNSUPPORTED`
(`0x80a80008`) because there is no recording backend. It does not retain or
claim to consume recording metadata; invalid arguments return their own error.

`tests/test_runtime_capture.c` checks hook availability, successful and failed
policy changes, reentrancy/retirement and the recording error contract. Android
instrumentation separately exercises the actual window flags and Activity
lifecycle; it does not need to run the game or the ARM64 runtime.

## AppContent

The provider remains an offline base-game profile. There are no installed
add-ons or granted entitlements. Queries, mounts and unmounts validate their
arguments and initialization state, return the corresponding absence errors,
and leave failed output buffers untouched.

The already configured private `/download0` mount supports real operations:

- Available-space queries return filesystem availability in KiB, not a fixed
  invented capacity.
- Format removes the contents of that mount and reports any filesystem error.
  It does not select a guest-supplied arbitrary path or remove savedata.
- On Android/Linux, directory descriptors and `O_NOFOLLOW` prevent traversing
  a symlink at the root or inside the tree. An internal link is removed while
  its target is preserved. The root directory stays mounted.
- Windows retains the existing physical-tree removal helper and rejects a
  reparse-point root before formatting. This path was source-reviewed; the
  focused test below executes the Linux path.

`DownloadDataFormat` accepts a pointer to a 16-byte mount structure containing
`/download0`; some reference headers omit this argument. Like a normal
filesystem deletion, a failed format can be partial and is not a transaction.

System-module reference logging no longer claims that every imported function
is implemented. The loader's independent import preflight determines coverage.

## RTC

ParseRFC3339 handles the full date/time, mandatory UTC designator or signed
offset, and fractional seconds scaled to microseconds. Numeric timezone offsets
are subtracted to obtain UTC. It validates the calendar, rejects malformed or
out-of-range input, and preserves the output tick on failure. Lowercase `t`/`z`
are accepted. Fraction digits beyond microsecond precision are truncated.
As with the existing RTC calendar, leap second 60 is rejected; no leap-second
table or separate time scale is claimed.

## Focused verification

From the repository root on Linux x86-64:

```sh
mkdir -p out
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -ffunction-sections -fdata-sections -pthread -I. \
  tests/test_runtime_services.c src/runtime.c src/runtime_services.c \
  src/runtime_ss.c src/runtime_content.c src/runtime_rtc.c \
  src/runtime_file.c src/runtime_cpu.c src/runtime_low_memory.c \
  -Wl,--gc-sections -o out/test-runtime-services
./out/test-runtime-services
```

The test resolves actual NIDs through the production component resolvers and
`runtime_lookup`. Unrelated renderer/kernel facilities are isolated; this is
not a substitute for the APK's fully linked resolver test. Assertions cover
state transitions, ownership, error paths, result canaries, no affirmative
selection, passive save progress, volume ABI, unavailable input, RTC bounds,
and real isolated filesystem operations including a symlink to a preserved
save. Test storage is a fresh temporary directory, never a real installation.

Public provider tests and optional local import analysis are described in
[README-import-audit.md](README-import-audit.md). Regenerate the public fixture
from the port's source bindings after changing exports. Keep any module-derived
inventory private and outside version control. Local diagnostics still report
all missing imports; known conditional gaps do not become implemented merely
because they are not expected to run offline.

## Contract references

Layouts, lifecycle and error constants were cross-checked against the upstream
projects' implementations and headers:

- [shadPS4 CommonDialog](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/system/commondialog.h),
  [MessageDialog](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/system/msgdialog_ui.h),
  [SaveDataDialog](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/save_data/dialog/savedatadialog_ui.h)
- [shadPS4 profile ABI](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/np/np_profile_dialog/np_profile_dialog.h)
  and the vendored `gpu/shadps4/core/libraries/np/np_commerce/np_commerce.cpp/.h`
- [OpenOrbis ErrorDialog ABI](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/error_dialog.h)
  and [AnyPS5 ErrorDialog lifecycle/error family](https://github.com/boykopovar/AnyPS5/blob/main/core/libs/prx/libSceErrorDialog/Export.cpp)
- [shadPS4 AppContent](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/app_content/app_content.cpp)
  and [error constants](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/app_content/app_content_error.h)
- [shadPS4 Voice ABI](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/voice/voice.h),
  [Voice implementation](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/voice/voice.cpp),
  [AudioIn](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/audio/audioin.cpp)
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339) and
  [shadPS4 RTC errors](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/rtc/rtc_error.h)
- [OpenOrbis screenshot ABI](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/ScreenShot.h)
  and [shadPS4 recording errors](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/video_recording/video_recording_error.h)
- [Android window capture policy](https://developer.android.com/reference/android/view/WindowManager.LayoutParams#FLAG_SECURE)

Reference coverage does not establish identical behavior for every console
firmware or unsupported service. Android UI, voice/capture and the remaining
social services are still limited as described above.
