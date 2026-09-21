# Native practice matchmaking — milestone 1

Base commit: `cfea2f7f994a1427a2c1c10bf2dfd81b7fc3cab2`

Branch: `feature/native-practice`

The supplied Preflight archive was inspected as a behavioral reference; it contains installers and
an xdelta payload, not reviewable source. The referenced SLP-TM checkout did not contain a license
file at the inspected commit, so this implementation does not copy its code. It independently uses
the fork's existing GPL-2.0-or-later Slippi interfaces and the observed high-level sequence only.

## Scope delivered

This milestone implements the Direct-code vertical slice:

- Tab opens a native ImGui matchmaking popup in normal menus and Training.
- Direct accepts keyboard typing and clipboard paste. Input is normalized to uppercase `NAME#digits` and limited to Slippi's 18-byte field.
- Controller navigation follows the human controller port found in Training. The UI waits for a neutral controller before accepting buttons.
- Starting a Direct search captures only the relevant Training configuration fields, starts the existing Slippi matchmaking implementation, and polls its side-effecting match state once per 60 Hz simulation tick.
- Closing the popup releases Training while search continues. A passive status indicator remains, and Tab reopens Cancel.
- Once both peers are ready, input is captured briefly, `Match found / Connecting...` appears, and the guest enters major scene 8 (the normal Slippi online flow). The overlay clears as soon as that scene is active, before the first online gameplay frame.
- Cancel invokes the existing Slippi connection cleanup. A 90-second timeout also cleans up.
- A pre-game disconnect shows `Disconnected — Press A to continue`. A held A is rejected until the selected controller is neutral and A is pressed again. The coordinator then requests Training and reapplies the saved character, costume, port, player/CPU kinds, CPU level, stage field, and percentages instead of restoring RAM.
- Once online gameplay reaches frame 1, ordinary Slippi reporting, disconnect, savestate, and set-flow code remains the owner. The practice coordinator does not replace it.

Ranked and Unranked are visible but disabled and explicitly labelled as not included in this milestone. Their UI is structured for later commands, but neither is presented as functional.

## Architecture and invariants

`native_practice.cpp` is the simulation-thread coordinator. `slippi::poll_options()` calls it once per retrace, before the guest advances. It is the only new caller of the side-effecting matchmaking poll and it stops polling when the normal online scene takes ownership.

The render thread reads an immutable `Snapshot` and enqueues Start, Cancel, or Acknowledge commands. It never calls networking or changes guest scene memory.

Training preservation is field-based. It does not save or restore whole RAM, so fresh netplay state is not overwritten. The cosmetic integration boundary is the read-only `slippi::native_practice::cosmetic_profile_locked()` hook; this branch has no cosmetic dependency.

Presentation settings are untouched. The diagnostic launcher uses 120 fps authored presentation while the coordinator stays on the existing 60 Hz retrace/simulation clock.

## Shared-file changes

These are the files most likely to overlap another feature branch:

| File | Additive change |
| --- | --- |
| `CMakeLists.txt` | Adds the portable lifecycle/validation test target. |
| `port/runtime/hle/exi_slippi.cpp` | Calls the coordinator once per retrace and shuts it down before Slippi. |
| `port/runtime/hle/slippi_online.h/.cpp` | Adds a narrow simulation-thread bridge around existing selection, find, match-state, and cleanup handlers. |
| `port/runtime/gx/pc_settings_shared.h` | Adds render-only popup/focus/input-gate state. |
| `port/runtime/gx/pc_settings.cpp` | Draws the popup/status/transition/failure UI and exchanges snapshots/commands. |
| `port/runtime/host/window.h/.cpp` | Reserves the Tab edge and exposes all four routed UI pads; capture now neutralizes every port rather than only port 1. |

## Windows build and diagnostic run

From a Visual Studio 2022 Developer PowerShell:

```powershell
./tools/native_practice_diagnostic.ps1 -Iso "C:/path/to/your-local-clean-ntsc-1.02.iso" -ConnectCode "NAME#123"
```

The script locally extracts/recompiles the ISO-derived guest, configures and builds `melee_port`, runs the portable model test, launches D3D12 at 120 fps authored presentation, and writes observed coordinator markers under `reports/native-practice-*`. Use `-Backend d3d11` for the second renderer pass or `-SkipBuild` after the first build. It never copies the ISO into the repository.

## Validation distinction

- Two-native-instance coverage uses `tools/online_pair.py` and verifies the native transport/simulation pair. Its local-peer mode bypasses public matchmaking and does not validate the Tab/Direct-code server path.
- Native-versus-stock-Slippi coverage must use a separately launched stock Slippi Dolphin peer and the real Direct code. That is the interoperability gate for this feature.

The portable model test covers connect-code validation, search/cancel/requeue lifecycle, successful handoff, pre-match failure/acknowledgement/return, and the post-frame-1 ownership boundary. The Windows game still needs the manual hardware/network matrix below; unrun items must not be marked passed:

1. D3D12 and D3D11 typing, Ctrl+V, controller focus, and selected ports 1–4.
2. Hold A while failure appears; release it; confirm only a new A acknowledges.
3. Repeated search/cancel cycles and the 90-second timeout cleanup.
4. Character, costume, controller port, CPU type/level, stage, and percentages after a post-handoff failure return.
5. Transition-overlay dismissal when scene 8 becomes active, with no input in the first gameplay frame.
6. Two native instances, then native versus current stock Slippi by Direct code.
7. Same-settings performance comparison at 60 and 120 presentations per second.

## Next implementation step

Run the Windows Direct-code matrix above and fix any scene-return or input-timing defects before
expanding matchmaking scope.

After that gate, add a Preflight-style CPU practice-controls milestone. Keep it confined to Training
and disable/remove its overrides before online simulation. The intended native popup controls are:

- CPU character, costume, behavior/type, and level;
- stage and starting percent configuration for both players;
- a quick position/percent reset that does not restart or duplicate matchmaking polling; and
- exact restoration of those settings after cancellation or a pre-match failure.

Confirm the exact Preflight behavior during implementation rather than copying its patch or assuming
undocumented options. Once the CPU-practice controls pass isolation and transition testing, add
Unranked's selection policy. Add Ranked last so ranked reporting and set flow can be validated
independently.
