# Notices

## Trademarks

Microsoft, Windows, and Windows XP are trademarks of Microsoft Corporation.
XPLogin10 is not affiliated with, endorsed by, or sponsored by Microsoft. The
names are used here only to describe what this software is compatible with and
what it looks like.

## Microsoft assets

This is a private build. `assets/` holds reference material copied out of a
Windows XP SP3 x86 installation - `logonui.exe`, `msgina.dll`, `winlogon.exe`,
the sounds and the stock account pictures - and the build reads the welcome
screen's original artwork straight out of it. That is the point of the project: the screen is a
port, not an impression.

Concretely:

* **`tools/xp-bake` extracts 25 bitmaps from two of XP's binaries** at build
  time - decoding them (including the RLE8 ones), applying each bitmap's own
  colour key, and writing `XPLogin.assets`. The installer embeds that file and
  deploys it beside the provider.

  From `assets/system32/logonui.exe`, 22: the logo, the two divider sweeps,
  the background glow, the account picture plate, the password field, the go
  and help buttons, the power icon.

  From `assets/system32/msgina.dll`, 3: the "Turn off computer" dialog is not
  part of the welcome screen. Clicking the power button opens a separate modal
  that msgina owns - which is why logonui's markup has no shutdown panel in it
  and none of that artwork is in its resources. Its background (#20142, the
  313x198 panel), the Windows flag in its title band (#20143) and the orb strip
  (#20150 - ten 32x32 frames: three states each for Turn Off, Stand By and
  Restart, plus a disabled Stand By) come from there.

* **The geometry and palette are Microsoft's too.** Every value marked
  `[UIFILE]` in `src/ui/include/xplogin/ui/XpTheme.h` is quoted from the
  DirectUI markup in the same binary, read with `tools/xp-extract`. Where the
  markup and the artwork disagree - bitmaps 109 and 110 declare no colour key
  but plainly use magenta - `src/assets/src/AssetPack.cpp` says so and explains
  which one won.

  The wording is Microsoft's as well. The big italic line on the logoff and
  shutdown screens comes from `winlogon.exe`'s string table - ids 1682, 1684,
  1685, 1686, 1687 and 1691 - because winlogon owns that sequence and pushes a
  status line to whichever host is on screen, which on XP is the welcome
  screen itself. That is also why logging off is not a separate screen there:
  it is this one with the account list gone and one line where the logo would
  be. `src/core/include/xplogin/ShellStatus.h` has the quotations.

  The shutdown dialog is marked `[DIALOG]` instead, because msgina draws it
  with a plain Win32 dialog rather than DirectUI: those values come from
  template #20100 (208x122 dialog units) put through the conversion its own
  background bitmap fixes - 208 x 6/4 = 312 and 122 x 13/8 = 198.25 against a
  313x198 panel.

* **Nothing is required at run time.** A build made without a reference
  `logonui.exe` produces no `XPLogin.assets`, and the renderer falls back to
  drawing the same screen from GDI+ primitives. A missing pack is never the
  reason somebody cannot sign in.

* **Sounds** are still opt-in: `[sounds]` in `XPLogin.ini` is empty by default
  and missing files are skipped silently. `tools/Import-XPSounds.ps1` copies
  them from a source you point it at and downloads nothing.

* **No fonts are bundled.** The theme asks for *Franklin Gothic Medium* and
  *Tahoma* by name and falls back to *Segoe UI* and then *Tahoma*.

### If you redistribute this

The extracted artwork is Microsoft's. Whether you may copy it out of a Windows
XP installation - and whether you may pass on the result - is governed by your
licence for that copy of Windows, not by this project. Building for a machine
you already licence is a different thing from shipping the output to others; if
you intend the latter, delete `assets/` and build without it. The primitives
path exists precisely so that build still works.

## Supplying your own assets

| What | Where to put it |
|---|---|
| The original artwork | `assets/system32/logonui.exe` - the build does the rest |
| Sounds | any folder; set the paths in `[sounds]`, or run `tools/Import-XPSounds.ps1` |
| Tile pictures | `[ui] avatardirectory`, named `<username>.png` or `.bmp` |
| A different look entirely | edit `XPLogin.theme.ini` - colours, gradients, metrics, fonts and strings, no rebuild needed |

## Third-party code

None. The automated tests use a small framework written for this repository
(`tests/framework/xptest.h`) rather than an external one; the PE reader, the BMP
decoder and the PNG encoder in `src/assets/` are written here rather than
pulled in from libpng or zlib. The whole project builds with nothing beyond a
C++17 compiler, CMake and the Windows SDK, and the installed product has no
runtime dependency beyond what Windows already ships.

## Security

This software runs inside `LogonUI.exe` and handles passwords. Two things worth
stating plainly:

* Typed passwords live in a `std::wstring` for as long as it takes to pack them
  into the buffer LSA expects, and are overwritten with `SecureClear` on every
  path out - after each attempt, on tile switch, on cancel, on re-initialisation
  and on shutdown. They are never logged, never written to disk, and never sent
  anywhere.
* Authentication is performed entirely by LSA. XPLogin10 does not compare
  passwords, does not read the SAM, and does not implement any part of the
  authentication protocol.
