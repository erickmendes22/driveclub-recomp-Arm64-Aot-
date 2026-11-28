# PKG and PFS parser provenance

`orbis-pkg` and `orbis-pfs` are vendored from
https://github.com/videobitva/orbis/tree/13f1294a87a53d54f82feec56acbfd03f50667a9 .
This is the same parser revision used by the Bionic PKG build in
`erickmendes22/gow3_android_daedalus_fork` at
`b3a7c3d5e916cf8f4622192c0faa9383ebd77c00`.

The upstream archive SHA-256 is
`0d455a4c258c6cde63bab323c2113a388ced199eabbaf2917330bb9aa9a40026`.
Upstream permits use under MIT or Apache-2.0; both license texts are included.
The parser includes the publicly documented fake-package keysets. It does not
provide console-specific retail content keys.

Driveclub's integration is a library called through JNI, not an extracted
executable. The wrapper validates the selected package's title, metadata,
paths, and supported patch mode before extraction, and writes only to a new
private installation generation. Local parser changes add bounds checks for
PFS/PFSC allocations, directory records, and unsupported inode layouts.
One unnecessary `const` qualifier is removed for the pinned Rust 1.85 compiler.
The source is kept in the repository so the Android build cannot silently
switch parser revisions.
