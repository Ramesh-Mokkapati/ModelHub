# RevEng stitch lessons


One place for what earlier sessions learned fixing stitched (Ghidra-decompiled) native projects, so a new session on another project starts from it. Per-feature status is in the RevEng README; the model setup is in the ModelHub README. The app's own LLM fix lessons are already shared by every project in `%LOCALAPPDATA%\RevEngStudio\knowledge\fix_lessons.jsonl` (written when learning auto-fix runs).

**Why:** the user wants fixes delivered across many projects (BatteryConsole, MfcThreadDemo, vdlkeygen, EnglishDLL, ...) without re-deriving the same causes.

**How to apply:** follow the loop, match the symptom to a pattern, check the traps, then add any new lesson here.

## Loop for a new project
1. Never touch the user's project folder: copy it to the scratchpad.
2. Fresh Ghidra output of a binary: `build\tools\ghidra\support\analyzeHeadless.bat <gp dir> <proj> -import <file> -scriptPath scripts\ghidra -postScript RevEngExportFunctions.java <functions.json> <functions dir> <types.h>` into `<project>\out\1\` (copy a `project.rsproj` from an earlier scratch project).
3. Re-stitch: `bin\Release\RevEngTests.exe --gtest_filter=NativeCauses.StitchReport` with env `REVENG_STITCH_PROJECT`, `REVENG_STITCH_KEEP=<out dir>`, optional `REVENG_STITCH_BINARY=<file>` (swaps the input in the scratch copy).
4. Build the output with VS2022 MSBuild (`...\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe <vcxproj> /p:Configuration=X /p:Platform=x64`). Always Debug AND Release.
5. Compare against the original by behaviour (stdout, exit code, window text via window messages, LoadLibrary/GetProcAddress for DLLs). Debugger-less crash hunting: a small dbghelp debugger (call stack + PDB lines + register dump) found every crash fast.
6. Edited Ghidra scripts are NOT redeployed by the build: copy by hand to `bin\Release\scripts\ghidra` and `bin-vs2022\Release\scripts\ghidra`; users must Re-analyze, then Regenerate.
7. Build the app tests with the VS2026 cmake (`MSYS_NO_PATHCONV=1`); after adding a test file re-run `cmake -S . -B build`.

## Fix patterns by symptom
- Compile errors from CRT/STL internals: leave runtime out by name (`IsRuntimeLibraryName`), link the real runtime; unnamed static-CRT code cannot be told apart (Ghidra FID has no match for new toolsets).
- Unresolved Windows types/functions: include SDK headers from the imports; link import `.def` libs; ordinal imports via Ghidra's `imports` + the compiler's atlmfc import lib.
- Crash at start: relocation fix-up of the PE image copy, `/GS` off, `/RTC1` off (BasicRuntimeChecks=Default), static initializers (`_initterm` tables), CFG `call rax` patch.
- Wrong output with printf-like calls: variadic wrappers marked in the Ghidra export BEFORE decompiling; stack-passed arguments restored; a dropped call argument (`_Mtx_lock`) restored by RestoreDroppedCallArguments.
- By-value struct return of a C++ import (`setprecision`): a sized `_Smanip`/`_Fillobj` only; never grow arbitrary demangled structs (a CString is 8 bytes).
- main/WinMain/DllMain not found: Ghidra flags `entry` as library, so scan the startup code (DllMain: `param_2 - 1U < 2` and `param_2 == 3`).
- DLL output: DynamicLibrary + exports.def (names, ordinals, forwarders), DllMain wrapper; exported data needs a symbol of its own: the image copy is laid out in pieces of one `.revimg` section, each exported address starting a piece (`reveng_export_<name>`, `DATA` in the .def), with a runtime order check.

## Limits still open
- Static-CRT programs (unnamed CRT code), 32-bit inputs (static library only), inlined DllMain, by-value struct returns of other C++ classes.

## Tool traps
- Python is not installed; use Edit/Write, awk or sed. A doubled backslash in a command arrives halved: escapes in scripted edits corrupt C++ sources. Never write an edit helper that converts line endings.
- Run `.bat` files via PowerShell `& "<full path>\x.bat"`; plain `cl` defaults to /MT, use /MD. `dumpbin -headers`, not `/headers`, under MSYS.
- git here reports "dubious ownership". The user commits; do not commit.
- Never kill unrelated processes. Drive GUI apps only through window messages.
- `for (auto& [k,v] : json.value("x", json::object()).items())` dangles: keep the json in a local.

## 32-bit (x86) inputs: what worked and what did not
- Build for Win32 (`Platform=Win32`, `/machine:x86`, x86 `cl` for the syntax check). Pointers in the image copy are 4 bytes (`REVENG_UPTR`).
- Ghidra prints no convention for x86's default `__stdcall`: write it out, or C compiles the function as `__cdecl` and Release builds crash (Debug's frame pointers hide the stack imbalance). A function that returns with `ret n` takes n/4 words even if the decompiler drops unused ones (DllMain's reserved): set them in the export script.
- printf wrappers: `&stack0x0000000c` is the va_list after k named words (4 + 4k). Mark variadic before decompiling (LEA of [ESP/EBP + 8..0x20] in a caller of `__stdio_common_v*`), and add call-site signatures from `add esp, n` where the decompiler shows fewer words (the compiler may schedule other instructions between the call and the cleanup; a double is two words).
- `__stdio_common_v*printf` take a 64-bit options argument as two words on x86: join with a macro.
- Ghidra shows imports without the leading underscore (`seh_filter_dll`): use the `original` name from the export's imports. Linker names are decorated: `/ALTERNATENAME:_alias=_name`; `_CxxThrowException` is `__stdcall` (`@8`).
- DllMain dispatcher on x86 reads `(param_2 == 1) || (param_2 == 2)`, not `param_2 - 1U < 2`.
- The CRT's own initializer tables point at unnamed functions on x86 (no FID match): treat data-reached functions that call startup APIs (`_initterm`, `_set_app_type`, `_configure_narrow_argv`, `func_0x...`) as startup code.
- C++ virtual calls work (x86, Debug and Release): `__thiscall` is not valid C, so write a member as `__fastcall f(void *this, int reveng_edx, ...)` and pass 0 for EDX in every call (direct, through a vtable slot, and the `(**(code **)*obj)(1)` virtual destructor, where `this` is missing). The words a virtual call pops are the `ret n` of the functions every vftable lists at that slot (export `virtualSlotPops`); the decompiler counts saved registers as arguments. A call-site override that guesses by counting pushes backwards does not work. Slot offsets come from the p-code data flow (LOAD of INT_ADD(vtable, n)); the compiler often loads the slot into a register first (`mov eax,[eax+4]; call eax`).
- The double-valued C++ test still fails, and the stitcher is not the cause: Ghidra conflates stack slots. One frame slot is both an element of the `shapes[]` pointer array and the double `total` (the x87/SSE spills are tracked at different ESP depths), so the decompiled code overwrites the pointers with a double. Bit-pattern fixes that were worth keeping: `slot = (double)(int expr)` on an `undefined8` slot is a bit pattern (drop the cast), `(float10)slot` reads it as a double, `(ulonglong)dVar` and `SUBxy(dVar, n)` take the double's bits (`REVENG_BITS`). The remaining fix belongs in Ghidra: fix the ESP tracking (stack depth after `sub esp, 8` / pushes around the call) or split the overlapping variables. `in_stack_00000004` is main's argc: make it a parameter. `main` stays `__cdecl`.
- Test programs: `cl /MD /O2 /Gy` x86 console exe with printf, doubles, qsort and a callback, and an x86 DLL with exports.
- Follow-up on the double-valued C++ test (investigated, not solved): Ghidra's listing has the right 8-byte locals (`local_15c`, len 8), but the decompiler places the double's store 4 bytes too high and writes its high half over `shapes[0]`. The listing's stack depth (CallDepthChangeInfo) is right; the decompiler's own stack tracking shifts after an indirect `call eax` / `call [esp+n]` whose callee-pop it has to guess (the same guess adds the stray `unaff_EDI` argument). Typing the array (`ArrayDataType` at -340, 3 pointers) and the doubles in the listing made the decompile cleaner but did not move the store, and it made another C++ test crash in Release (heap corruption), so it was removed. A call-site override only helps when it is applied to every indirect call of the function (the one through `[esp+0x10]` is missed: ECX was loaded nine instructions earlier, the look-back is six) and its `__thiscall` signature must give the callee's pop (the vftable slot's `ret n`). Next try: apply the slot-pop signature to every indirect call (also those through a stack slot, by following the p-code), then check whether the shift disappears.
- Tried: look back 30 instructions (not 6) for the ECX load so the `call [esp+0x10]` is overridden too. All three virtual calls were then overridden, but the decompiler still shows extra stray arguments (`SUB84(dVar13,0)`, `unaff_EDI`) on the slot-1 calls: the override's one-parameter `__thiscall` signature does not remove them (the slot-2 call through `esi` does come out with `this` only). Reverted to 6. What is not understood: why a call through `eax` keeps a stack parameter and one through `esi` does not. Probably needs the call's stack-purge (extrapop) set explicitly, or a prototype with custom storage (ECX + nothing on the stack), neither of which `writeOverride` with a plain FunctionDefinition gives. The C++ programs without floating-point work.
