# Native practice matchmaking — milestone 1

Base commit: `cfea2f7f994a1427a2c1c10bf2dfd81b7fc3cab2`

Branch: `feature/native-practice`

The supplied Preflight archive was inspected as a behavioral reference; it contains installers and
an xdelta payload, not reviewable source. The referenced SLP-TM checkout did not contain a license
file at the inspected commit, so this implementation does not copy its code. It independently uses
the fork's existing GPL-2.0-or-later Slippi interfaces and the observed high-level sequence only.

## Scope delivered

This milestone implements the Direct-code vertical slice and native Unranked search:

- Tab opens a native ImGui matchmaking popup in normal menus and Training.
- Direct accepts keyboard typing and clipboard paste. Input is normalized to uppercase `NAME#digits` and limited to Slippi's 18-byte field.
- Controller navigation follows the human controller port found in Training. The UI waits for a neutral controller before accepting buttons.
- Starting Direct or Unranked captures only the relevant Training configuration fields, starts the existing Slippi matchmaking implementation, and polls its side-effecting match state once per 60 Hz simulation tick.
- Unranked carries the Training character/costume into Slippi's existing fixed-rules matchmaking flow. Direct continues to use a typed or pasted connect code.
- Closing the popup releases Training while search continues. A passive status indicator remains, shows elapsed search time from the 60 Hz coordinator clock, and Tab reopens Cancel.
- Once both peers are ready, input is captured briefly, `Match found / Connecting...` appears, and the guest enters major scene 8 (the normal Slippi online flow). The overlay clears as soon as that scene is active, before the first online gameplay frame.
- Cancel invokes the existing Slippi connection cleanup. A 90-second timeout also cleans up.
- A pre-game disconnect shows `Disconnected — Press A to continue`. A held A is rejected until the selected controller is neutral and A is pressed again. The coordinator then requests Training and reapplies the saved character, costume, port, player/CPU kinds, CPU level, stage field, and percentages instead of restoring RAM.
- Once online gameplay reaches frame 1, ordinary Slippi reporting, disconnect, savestate, and set-flow code remains the owner. The practice coordinator does not replace it.

Ranked remains visible but disabled. Unranked is enabled but requires Windows/public-matchmaking validation; unavailable peer testing must remain reported as unrun rather than inferred from the shared Slippi path.

## Architecture and invariants

`native_practice.cpp` is the simulation-thread coordinator. `slippi::poll_options()` calls it once per retrace, before the guest advances. It is the only new caller of the side-effecting matchmaking poll and it stops polling when the normal online scene takes ownership.

The render thread reads an immutable `Snapshot` and enqueues Start, Cancel, or Acknowledge commands. It never calls networking or changes guest scene memory.

Training preservation is field-based. It does not save or restore whole RAM, so fresh netplay state is not overwritten. The cosmetic integration boundary is the read-only `slippi::native_practice::cosmetic_profile_locked()` hook; this branch has no cosmetic dependency.

Presentation settings are untouched. The diagnostic launcher uses 120 fps authored presentation while the coordinator stays on the existing 60 Hz retrace/simulation clock.

## Shared-file changes

These are the files most likely to overlap another feature branch:

| File | Additive change |
| --- | --- |
| `build.bat` | Supports a no-pause flag used only by the one-click wrapper. |
| `CMakeLists.txt` | Adds the portable lifecycle/validation test target. |
| `port/runtime/hle/exi_slippi.cpp` | Calls the coordinator once per retrace and shuts it down before Slippi. |
| `port/runtime/hle/slippi_online.h/.cpp` | Adds a narrow simulation-thread bridge around existing selection, find, match-state, and cleanup handlers. |
| `port/runtime/gx/pc_settings_shared.h` | Adds render-only popup/focus/input-gate state. |
| `port/runtime/gx/pc_settings.cpp` | Draws the popup/status/transition/failure UI and exchanges snapshots/commands. |
| `port/runtime/host/window.h/.cpp` | Reserves the Tab edge and exposes all four routed UI pads; capture now neutralizes every port rather than only port 1. |

## Windows build and diagnostic run

For the simplest build-and-test flow, double-click
`BUILD_AND_PLAY_NATIVE_PRACTICE.bat`. It opens an ISO file picker when needed, builds the Release
executable, runs the native-practice model test, then launches the 120 fps diagnostic session.
The clean ISO stays local and is not copied or modified.

From a Visual Studio 2022 Developer PowerShell:

```powershell
./tools/native_practice_diagnostic.ps1 -Iso "C:/path/to/your-local-clean-ntsc-1.02.iso" -ConnectCode "NAME#123"
```

The script locally extracts/recompiles the ISO-derived guest, configures and builds `melee_port`, runs the portable model test, launches D3D12 at 120 fps authored presentation, and writes observed coordinator markers under `reports/native-practice-*`. Use `-Backend d3d11` for the second renderer pass or `-SkipBuild` after the first build. It never copies the ISO into the repository.

## Validation distinction

- Two-native-instance coverage uses `tools/online_pair.py` and verifies the native transport/simulation pair. Its local-peer mode bypasses public matchmaking and does not validate the Tab/Direct-code server path.
- Native-versus-stock-Slippi coverage must use a separately launched stock Slippi Dolphin peer and the real Direct code. That is the interoperability gate for this feature.

The portable model test covers connect-code validation, elapsed-time formatting, search/cancel/requeue lifecycle, successful handoff, pre-match failure/acknowledgement/return, and the post-frame-1 ownership boundary. The Windows game still needs the manual hardware/network matrix below; unrun items must not be marked passed:

1. D3D12 and D3D11 typing, Ctrl+V, controller focus, and selected ports 1–4.
2. Hold A while failure appears; release it; confirm only a new A acknowledges.
3. Repeated search/cancel cycles and the 90-second timeout cleanup.
4. Character, costume, controller port, CPU type/level, stage, and percentages after a post-handoff failure return.
5. Transition-overlay dismissal when scene 8 becomes active, with no input in the first gameplay frame.
6. A real Unranked public match, two native instances, then native versus current stock Slippi by Direct code.
7. Same-settings performance comparison at 60 and 120 presentations per second.

## Next implementation step

Run the Windows Unranked queue/cancel/timer test and the Direct-code matrix above, then fix any
scene-return or input-timing defects before expanding matchmaking scope.

After that gate, add a Preflight-style CPU practice-controls milestone. Keep it confined to Training
and disable/remove its overrides before online simulation. The intended native popup controls are:

- CPU character, costume, behavior/type, and level;
- stage and starting percent configuration for both players;
- a quick position/percent reset that does not restart or duplicate matchmaking polling; and
- exact restoration of those settings after cancellation or a pre-match failure.

The follow-on CPU roadmap may add replay-derived skill profiles using authorized local `.slp`
datasets joined to a rating/ranking source such as Lucky Stats. Replays are training data, not input
scripts: an offline pipeline should extract state/action examples and aggregate them by character,
matchup, and skill cohort, then export a small deterministic runtime policy. Proposed presets are
Tech Fundamentals, Defensive, Offensive, Local PR, Regional Demon, Top 100, and Top 10. The last
four names describe source-data cohorts until human sparring tests calibrate their actual strength;
they must not be marketed as literal simulations of ranked players.

Treat drill style and opponent strength as separate controls. Tech Fundamentals, Defensive, and
Offensive select which situations the CPU emphasizes; Local PR, Regional Demon, Top 100, and Top
10 select a replay-derived performance band. Calibrate those bands with measurable outcomes rather
than fixed reaction-speed labels, including:

- L-cancel success rate by character and aerial;
- tech-chase continuation/hit rate by knockdown state and available tech options;
- conversion rate, with the damage or stock threshold stated explicitly;
- damage per opening and stock-conversion rate;
- edgeguard conversion and recovery success;
- DI/SDI and defensive-tech quality; and
- observation-to-action latency, execution-error rate, and option diversity.

Define an opening and its end condition once in the offline analyzer so the same event cannot be
counted differently between cohorts. Estimate bands per character and matchup, use held-out replays,
and apply minimum-sample/shrinkage rules before displaying a tier. Runtime validation should report
the same metrics. Never improve a tier by reading the current controller input or otherwise giving
the CPU information a human opponent would not yet have; difficulty should come from the learned
policy and calibrated error/latency distributions.

Start with one well-covered character/matchup pilot. Validate reaction delay, execution error,
option diversity, DI/SDI/tech choices, recovery, and punish behavior before expanding character
coverage. Use a scripted safety layer for recovery and invalid states because replay imitation alone
can drift into situations absent from its training examples. Dataset acquisition must use an
approved export/API or user-supplied files; do not depend on unapproved bulk scraping. Model
training remains offline, and the practice policy must be unloaded before any online simulation.

Confirm the exact Preflight behavior during implementation rather than copying its patch or assuming
undocumented options. Keep Ranked last so ranked selection, reporting, and set flow can be validated
independently.
