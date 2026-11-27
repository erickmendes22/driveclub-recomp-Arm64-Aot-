# Pinned Android Vulkan loader

Runtime sources are vendored from these BSD-2-Clause projects:

- libadrenotools: https://github.com/bylaws/libadrenotools/tree/8fae8ce254dfc1344527e05301e43f37dea2df80
- liblinkernsbypass: https://github.com/bylaws/liblinkernsbypass/tree/aa3975893d83ef1bc84c321ec60c65fbf1287887

Archive SHA-256 values:

- libadrenotools: `ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb`
- liblinkernsbypass: `da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638`

The upstream licenses remain in `LICENSE` and `lib/linkernsbypass/LICENSE`.
The Android build uses the runtime sources directly and builds only the hooks
needed for custom driver loading. File redirection and GSL mapping are not enabled.

Driveclub changes to the pinned sources:

1. Custom-driver requests cannot take the upstream system-driver fallback.
2. `HookImplParams` records whether the requested custom library was opened.
3. `adrenotools_custom_driver_loaded(loader)` exposes that result so a valid loader
   handle alone cannot be mistaken for successful custom-driver selection.
4. The linker helper checks namespace creation and memory-protection failures,
   bounds its ARM64 instruction scan, and applies the BTI protection change to
   the target code page instead of the local function-pointer variable.
5. The unique loader name reserves space for the three digits and terminating
   null byte; the hook validates nullable filename/extension-info arguments.

The helper is loaded only for an explicit custom-driver request. System-driver
selection does not initialize the linker-namespace helper. Loader handles are
retained for the process lifetime.
