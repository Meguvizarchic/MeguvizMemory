# C++ convenience static evidence — 2026-11-09

This static bundle records completed host, C++ ASan, installed-header, compile-contract and Android C++ compatibility checks. Its baseline is commit `7f076e5`; the native core remains byte-identical source (`8628ce7f…`).

- Host core/JNI: 48/48; C++: 15/15; ASan: 48/48; C++ ASan: 15/15.
- Installed C++ consumer: 1/1, with 14 RAII cases. Generated include paths and the loaded DLL were read back; local paths are hashed in the public copy.
- Strict MSVC Release /O2 /W4 /WX and Debug /Od /W4 /WX controls passed for recursive READ functors, callback/caller try-catch, function pointers and address-taken acquire_borrow. Actual MSBuild flags were read back, with no C4714 suppression and no lowered warning level.
- One positive compile control and two intended compiler rejections verify const READ callbacks and status return types. These two nonzero compiler exits are successful negative tests, not ordinary build failures.
- Android NDK29/API27/static: one newly linked consumer ELF and three compiled AArch64 REL translation units. The previously verified, exact-source-bound core SO is explicitly reused; it was not rebuilt in this check. New consumer ABI, 16 KiB LOAD, dependencies, no RPATH and safe strip were checked. No target/device execution or SDK publication occurred.
- `production.patch` contains five header/test/CMake changes with zero context, excluding documentation; the baseline header was checked against Git, and reverse application against the current frozen files passed.
- `frozen-sources.zip` contains exact immutable source copies. It excludes the still separately managed performance probe.
- Private absolute path strings and map keys are replaced by their UTF-8 SHA256, with original local receipt byte identities retained. These JSON copies are sanitized reports, not byte-identical original receipts.
- The full-chain candidate had no pre-native preparation error. Earlier candidates and their errors remain separately archived and are not inherited as this attempt.
- Original logs and private receipts remain locally unchanged. No performance result is included in this static index; timing and the final adoption decision are separate root-task records.

The source archive is evidence, not a complete installable SDK. Host/static compatibility, actual device execution and product performance remain distinct evidence scopes.
