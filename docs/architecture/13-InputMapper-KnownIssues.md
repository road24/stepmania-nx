# InputMapper: joystick auto-mapping doesn't survive a game switch

## Bug

Switching the active game (e.g. Dance → Pump) never re-runs joystick
auto-mapping for the newly active game. A controller only gets its
`g_AutoMappings` entry applied/saved if it happens to be connected the
*one time* `CheckForChangedInputDevicesAndRemap()` runs at boot
(`StepMania.cpp:1287`), scoped to whichever game is active at that exact
moment (`PREFSMAN->GetCurrentGame()`, default "dance").

## Why

- `AutoMapJoysticksForCurrentGame()` (`InputMapper.cpp:699`) is only ever
  called from that one boot-time check, gated on
  `g_sLastSeenInputDevices` (a `Prefs.ini` value) differing from the
  currently-connected device list.
- `GameLoop::DoChangeGame()` (`GameLoop.cpp:234-238`) switches
  `InputMapper::m_pInputScheme` and calls `ReadMappingsFromDisk()`, which
  only reads existing `Keymaps.ini` + fills keyboard defaults
  (`AddDefaultMappingsForCurrentGameIfUnmapped()`, hardcoded to
  `DEVICE_KEYBOARD`). Neither path re-triggers joystick auto-mapping.
- Confirmed this isn't new/Switch-specific: `git show a281b601` (original
  Switch auto-map commit) and `9bbaa938` (making the manual
  `ScreenMapControllers` screen controller-navigable) both work around
  this same limitation rather than fix it — mainstream StepMania's
  intended path for non-default games is manual configuration via
  `ScreenMapControllers`, which does correctly save
  (`ScreenMapControllers.cpp:782`).

## Candidate fixes (not yet implemented)

1. Add a joystick equivalent of `AddDefaultMappingsForCurrentGameIfUnmapped()`,
   called from `ReadMappingsFromDisk()`: for the current game, if a
   connected device has a `g_AutoMappings` entry and none of its buttons
   are already mapped (`IsMapped()`), apply it and `SaveMappingsToDisk()`.
2. Or: clear/reset `g_sLastSeenInputDevices` on game change so the
   existing boot-time check re-fires for the new game.

Attempted #1 in-session; reverted before landing (uncommitted, no residue
left in `InputMapper.cpp`/`.h`).
