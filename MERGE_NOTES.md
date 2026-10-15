# Upstream sync notes

Branch `lylat/upstream-sync`, based on `origin/slippi` (b389d70167,
"release(netplay): v4.0.0-mainline-beta.1", October 2023).

## Merge

`upstream/slippi` (project-slippi/dolphin, 9d13b39188) was 4611 commits ahead
and the branch had no commits of its own past the base, so
`git merge-base --is-ancestor HEAD upstream/slippi` held and the merge was a
fast-forward (`git merge --ff-only upstream/slippi`). No merge commit, no
conflicts, nothing to resolve.

Lylat features: `origin/slippi` on project-lylat/lylat-ssbm-dolphin is a plain
copy of an older Slippi mainline. It has no Lylat backend URLs, matchmaking
changes or hole punch of its own (no commits past the Slippi history), so the
fast-forward drops nothing. The other remote branches (`ci-speedup`,
`fix/macos-ci`, `linux-fixes`, `upstream-master`, `upstream-merge-2023-09-06`)
were not part of this branch and were not merged.

Submodules: after the fast-forward, `git submodule update --init --recursive`.
`Externals/spirv_cross/SPIRV-Cross` checks out with one test file missing
(`reference/opt/shaders-msl/comp/overlapping-bindings...comp`, a path longer
than Windows allows). It is a SPIRV-Cross test reference and is not built.

## Build fixes after the merge

- `CachedInterpreter.cpp`: MSVC 19.36 (VS 17.6, the compiler on this machine)
  rejects `cond ? CallbackCast(a) : CallbackCast(b)` with C7595 ("call to immediate
  function is not a constant expression"). Rewritten as if/else with the same
  calls. Newer MSVC (the CI's) accepts the original; behaviour is the same.

## QR sign-in (added on top)

Slippi login on mainline lives in the Rust extensions
(`Externals/SlippiRustExtensions/user/src/lib.rs`: `open_login_page` opens
`https://slippi.gg/online/enable?path=<user.json>` and a watcher thread loads
`user.json`). The submodule is not changed. The hook is on the C++ side:

- `Source/Core/Core/Slippi/SlippiQrLogin.{h,cpp}`: the hook. Holds the UI
  handler and the running EXI device's `SlippiUser` (for the browser fallback).
- `CEXISlippi::handleLogInRequest` (`EXI_DeviceSlippi.cpp`): when the game asks to
  log in and `user.json` is missing, `SlippiQrLogin::Start()` runs before
  `OpenLogInPage()`. If a QR sign-in server is set, the Qt window opens instead
  of the browser. `ListenForLogIn()` runs either way, so the Rust watcher loads
  the `user.json` the window writes.
- `Source/Core/DolphinQt/SlippiQrLoginDialog.{h,cpp}`: the Qt window and the
  device-code client (`POST <origin>/api/device/code`, then `/api/device/token`).
  Writes `user.json` in `File::GetUserPath(D_SLIPPI_IDX)`, the folder the Rust
  side watches (temp file, then rename).
- Setting `[Slippi] QrLoginServer` in `Dolphin.ini` (`Config::SLIPPI_QR_LOGIN_SERVER`),
  also in Settings > Slippi > Online Settings with a "Sign In..." button. Empty by
  default: mainline has no Lylat backend setting, so the Slippi browser login stays
  the default.
- 404 from `/api/device/code`: the window closes and the browser login opens
  (from Settings, with no game running, it shows an error instead).
- Redirects: the device-code client does not follow them. `Common::HttpRequest`
  sets `CURLOPT_FOLLOWLOCATION` only in `FollowRedirects()`, which the dialog never
  calls, and libcurl's default is off.
- Cancel: `StopAttempt()` sets the cancel flag under the attempt's mutex before
  notifying, so the polling thread cannot miss the wakeup.
- `Externals/qrcodegen`: Nayuki's QR Code generator (C, MIT, v1.8.0), copied from
  doldecomp `native/source/vendor/qrcodegen`, with its LICENSE; listed in
  `Externals/licenses.md`.

## Build status (2026-10-13)

Release x64, Ninja, `-DSLIPPI_PLAYBACK=false`, `cmake --build <tmp>/build --target dolphin-emu -j 2 -- -k 0`
(script: `..	mp-lylat-ssbm-dolphinuild.bat`, run under heavy.lock).

- Run 1: stopped on the C7595 error above (exit 2).
- Run 2: no compile errors, then `LNK1180` (disk full) at step 362/588 (exit 2).
- Run 3 (after disk space was freed): exit 0, `Binary/x64/Slippi_Dolphin.exe` built
  (the version string reads 4.0.0-mainline-beta.19). No further code fixes were needed.

## QR sign-in check (2026-10-13, local mock only)

Mock: `..	mp-lylat-ssbm-dolphin\mock\mock_server.py --port 3471 --pending 8 --interval 2`
(code `MOCK-7Q4X`, pick 42, QR for `http://mock.invalid/device?code=MOCK-7Q4X`).
Dolphin was started with `-u <tmp user dir>`, `[Slippi] QrLoginServer = http://127.0.0.1:3471`
and the test aid `DOLPHIN_SLIPPI_QR_LOGIN_TEST=1`, which opens the window at startup.
Shots are in `..	mp-lylat-ssbm-dolphin\shots\`.

| Check | Result |
| --- | --- |
| Code request | the mock logged one `/api/device/code` with client `slippi-dolphin`, platform `windows` |
| Showing (`qr-showing.png`) | QR, `MOCK-7Q4X`, "Pick 42 on your phone", address, "Expires in 9:57" |
| Polling | `/api/device/token` every 2 s; pending for 8 s, then 200 |
| Approved (`approved.png`) | "Approved! Signing you in..." |
| Logged in (`logged-in.png`) | "Logged in"; `Slippi/user.json` in the user dir is byte-for-byte the mock's account |
| Denied (`error-denied.png`) | "Sign-in denied" with Retry; no user.json written |

Not verified:
- The in-game path (`handleLogInRequest` from Melee's Login, with the EXI device
  running). No game was booted. Without a device, Manual login is hidden, and the
  Approved state waits the full 10 s for the Rust watcher before showing Logged in.
- That the Rust watcher loads the written user.json in a running game.
- The 404 fallback to the browser, Retry, slow_down, expired and the server-error states.
- A decode of the shown QR (OpenCV is not installed here; the QR is drawn from
  `verification_uri_complete` with qrcodegen).
- A real Lylat server and a phone scan.
