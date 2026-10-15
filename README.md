# FC4 ForcePad: controller-only Far Cry 4 (Legion Go 2 and other handhelds)

A drop-in DLL that makes Far Cry 4 use your controller and ignore keyboard and mouse.
It fixes the game showing keyboard prompts (Space, Esc and so on) and misreading inputs on
handhelds like the Lenovo Legion Go 2. On those devices the touchpad, gyro-mouse or
back-paddle key mappings keep sending keyboard and mouse events, so the game keeps switching
back to keyboard/mouse mode.

## What it does
- **Hides keyboard and mouse input from the game.** It covers window messages, Raw Input,
  DirectInput keyboard/mouse and `GetAsyncKeyState`. The game only ever sees the gamepad, so it
  stays in controller mode with controller button prompts.
- **Makes your controller player 1.** If Windows put the built-in controller in XInput slot 2–4,
  or a virtual pad from Steam or Legion Space grabbed slot 1, the game would ignore your real
  controller. The mod shows the game the first *connected* controller as slot 1.
- **Freezes and parks the mouse cursor**, so a cursor in the middle of the screen can't steal focus in menus.
- **Toggle while playing:** hold **LB + RB + View** (the two-squares button) for 1.5 s to switch
  the keyboard/mouse blocking on or off. You'll hear a beep.

It changes no game files and is removed by deleting the DLL.

## Install
1. Download `xinput1_3.dll` and `ForcePad.ini` from the `dist` folder.
2. Copy both into the Far Cry 4 **`bin`** folder, the one containing `FarCry4.exe`:
   - Ubisoft Connect: `C:\Program Files (x86)\Ubisoft\Ubisoft Game Launcher\games\Far Cry 4\bin`
   - Steam: `...\steamapps\common\Far Cry 4\bin`
   - Epic: `C:\Program Files\Epic Games\FarCry4\bin` (or wherever you installed it)
3. Start the game.

If a game update or a file check puts back an `xinput1_3.dll` there, copy the mod's file in again.

### Check it's working
After launching once, there should be a `ForcePad.log` in the `bin` folder. It lists what was
hooked and, every 30 s, how many keyboard/mouse events were blocked.

**No `ForcePad.log` appears?** The game didn't load the DLL under that name. Rename
`xinput1_3.dll` to `xinput9_1_0.dll`, then to `dinput8.dll` if that doesn't work either, and
try again. Use only one name at a time. The same DLL works under any of these names.

## Settings (`ForcePad.ini`)
| Setting | Default | Meaning |
|---|---|---|
| `Enabled` | 1 | 0 = the mod does nothing (plain pass-through) |
| `BlockKeyboard` | 1 | Hide keyboard input from the game |
| `BlockMouse` | 1 | Hide mouse, touchpad and gyro-mouse input |
| `FreezeCursor` | 1 | The game always sees the cursor in one fixed spot |
| `ParkCursor` | 1 | Move the cursor to the bottom-right corner on start |
| `PadSlot` | -1 | -1 = auto. 0–3 forces a specific XInput slot |
| `HideOtherPads` | 1 | Show the game only one controller |
| `ToggleCombo` | 1 | Enable the LB+RB+View toggle |
| `Log` | 1 | Write `ForcePad.log` |

If the wrong device is picked as the controller, set `PadSlot` to 0, 1, 2 or 3.

## Legion Go 2 settings that also help
In **Legion Space**, for Far Cry 4 (or globally):
- Use the controller's standard **XInput / Xbox** mode.
- Turn **off FPS mode** and any "mouse mode" on the right stick, gyro or touchpad.
- Remove any **keyboard key mappings** on the back paddles or extra buttons. Those are what
  send "Space" / "Esc" to the game.

If you launch through Steam, you can also try turning Steam Input on or off for Far Cry 4 under
*Properties → Controller*. The mod works either way, but leaving `HideOtherPads=1` on matters if
Steam creates an extra virtual controller.

## Uninstall
Delete `xinput1_3.dll` (or whatever you renamed it to), `ForcePad.ini` and `ForcePad.log` from the `bin` folder.

## Building from source
Requires mingw-w64 (`sudo apt install gcc-mingw-w64-x86-64`), then run `./build.sh`. The output goes to `dist/`.
Source: `src/forcepad.c` and the export list in `src/forcepad.def`.
