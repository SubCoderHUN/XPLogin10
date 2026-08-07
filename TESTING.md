# Testing XPLogin10

Two halves. The automated suite covers everything that can run without a logon
session; the manual checklist covers the part that cannot - a real Winlogon
desktop, a real LSA, a real reboot.

> **Never run the manual tests on a machine you need.** A credential provider
> that fails at the wrong moment leaves a machine that boots to a logon screen
> nobody can get past. Everything below assumes a throwaway VM with a
> checkpoint.

---

## Part 1 - Automated tests

### Running them

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cd build && ctest --output-on-failure
```

On Windows, `.\tools\Build.ps1` does the same and also builds the Win32 targets.

```
100% tests passed, 0 tests failed out of 26
```

435 tests, none of which needs a Windows machine.

CTest registers the whole binary once (`xplogin_all`) plus one entry per suite,
so a failure names the area. The binary can also be driven directly:

```bash
./build/bin/xplogin_tests                       # everything
./build/bin/xplogin_tests --filter StateMachine. # one suite
./build/bin/xplogin_tests --filter Unlock        # anything matching
./build/bin/xplogin_tests --list                 # names only
./build/bin/xplogin_tests --verbose              # print passes too
```

Exit code is 0 or 1; a failure prints the file, line, actual and expected.

### What the automated tests cover

**Unit - authentication logic** (`tests/unit/test_authentication.cpp`)
Valid password, invalid password, non-existent user, wrong domain, blank-password
accounts, disabled and locked-out accounts checked *before* the password, case
sensitivity (passwords yes, user names no), lockout counting and its reset, the
asynchronous LSA handshake, and the full NTSTATUS → message mapping - including
that `STATUS_LOGON_FAILURE` without a substatus is deliberately reported as "bad
password" rather than leaking whether the account exists.

**Unit - credential packing** (`tests/unit/test_credential_packer.cpp`)
The exact `KERB_INTERACTIVE_UNLOCK_LOGON` byte layout, pinned at both 32- and
64-bit pointer widths, verified against the real SDK struct when built on
Windows. Round trips, non-ASCII and non-BMP passwords, empty domain/password,
the `USHORT` length ceiling, and every bounds check LSA performs before trusting
a blob - truncation, offsets pointing into the header, odd byte counts.

**Unit - UI state machine** (`tests/unit/test_state_machine.cpp`)
Lock, login and shutdown states, as required, plus every transition that must be
*refused*: selecting a disabled account, submitting with nothing selected, the
back button during an unlock, opening the shutdown dialog mid-authentication,
shutdown when policy forbids it. Also the error/retry cycle, the max-attempts
fallback, and what happens when an account disappears while its tile is
selected.

**Unit - crash-loop recovery** (`tests/unit/test_health_monitor.cpp`)
Every rung of the ladder: clean → one strike → unfilter → bypass, Safe Mode
always bypassing, the disabled flag, configurable thresholds, recovery after a
single good logon, and correct behaviour when the registry is unreachable.

**Unit - which sign-in tiles get hidden** (`tests/unit/test_signin_options.cpp`)
The filter is the one object here that can make a machine unusable, so its rule
lives in portable code. Classification of every credential provider Windows
ships; that an unrecognised one defaults to "leave it alone" rather than
"hide"; each of the three modes; the configured allow list; and the two cases
where filtering is abandoned outright rather than risking a lockout - nothing
would remain, or XPLogin's own provider is missing from the list because
Windows could not load the DLL. Also that the shipped default is the safe mode,
and that a typo in `signinoptions` falls back rather than guessing. Also when a
machine signs itself in: one account with no password and nothing else, never
two, never a disabled or locked-out one.

**Unit - registry plan** (`tests/unit/test_registration_plan.cpp`)
Every key the installer writes, the `Apartment` threading model, uninstall being
the exact inverse and removing the filter *before* the provider, cleanup steps
being optional, and the safety gate - including that it rejects deleting the
whole `Winlogon` key even though that key is on the allow list.

**Unit - theme, layout, hit testing, animation** (`tests/unit/test_ui_layout.cpp`)
Gradient interpolation and clamping, the Luna palette, uniform scaling at
1024×768 / 1280×1024 / 1920×1080, bands tiling the screen with no gaps, tiles
never overlapping, avatars staying inside their tile, controls not overlapping,
the shutdown dialog being centred, clicks landing on the right control, the
modal swallowing background clicks, scrolling a long user list, and the
ease-out curve staying an ease-out.

**Unit - installer and uninstaller** (`tests/unit/test_deployment_plan.cpp`)
The file plan (every payload file copied, the uninstaller copied in beside it so
Programs and Features still has something to run later), the reverse plan
(everything removed, plus the logs and the recovery `.reg`, directory last and
only if empty), and the guard: `C:\Windows`, `C:\Program Files`, `C:\` and
relative paths are all refused as install roots, a target outside the root fails
validation, and `C:\XPLogin10Extra` is correctly *not* inside `C:\XPLogin10`.
Also the Programs and Features entry, the quoting of `UninstallString`, the
ordering rules (restore point before any change, recovery file before the
registry, files before registration, registry removed before files) - each
with a deliberately broken plan proving the check is not vacuous - and the four
System Restore outcomes.

**Unit - which XP status screen appears** (`tests/unit/test_shell_status.cpp`)
The mapping from state and power action to *welcome* / *Logging off…* /
*Saving your settings…* / *Windows is shutting down…* / *Preparing to stand
by…* / *Preparing to hibernate…*, and which states show no status screen at
all. The one that reaches a screen is *welcome*, between the password being
accepted and the desktop appearing.

**Unit - configuration** (`tests/unit/test_config.cpp`)
Sections, case insensitivity, comments, quoting, CRLF, UTF-8, colour formats,
and - most importantly - that malformed values fall back to the default instead
of being half-read, and that one bad line does not blank out the rest of the
file.

**Integration - dynamic account enumeration** (`tests/integration/test_user_enumeration.cpp`)
That the tile list is genuinely re-queried: an account added after the first
refresh appears, a deleted one disappears. Local and domain accounts merged, the
same name in two domains kept separate, deduplication by SID, system accounts
hidden, Guest hidden, the built-in Administrator hidden *unless it is the only
way in*, `SpecialAccounts\UserList` honoured, stable ordering across refreshes.
Also how a Microsoft account is classified: `MicrosoftAccount\` and `AzureAD\`
are their own kind rather than domain accounts, the switch about domains cannot
hide one, the switch about them can, and the qualified name survives to LSA
intact - the email half on its own authenticates nothing.

**Integration - session handling** (`tests/integration/test_session_simulation.cpp`)
A simulated Terminal Services session table driven through first logon (shell
started exactly once, never twice), Win+L, unlock with the wrong then the right
password, fast user switching (returning user reconnects to their existing
session and keeps their programs; a different user gets a new one), logoff, and
that a failed logon never touches the session table at all.

**Integration - the whole flow** (`tests/integration/test_logon_flow.cpp`)
Click → type → Enter, the credential-provider handshake (serialization is only
available while authenticating; `ReportResult` delivers the outcome; a stray
`ReportResult` cannot fake a logon), the unlock message type, the shutdown
dialog, sounds, and that the typed password is wiped after every attempt, on
tile switch, on cancel and on re-initialisation.

### Adding a test

`tests/framework/xptest.h` is a ~200-line framework with no external
dependencies. A test is one macro:

```cpp
TEST(SuiteName, DescribesWhatIsTrue) {
    CHECK_EQ(Actual(), Expected());   // records and continues
    REQUIRE(pointer != nullptr);      // aborts this test
}
```

Add the file to `tests/CMakeLists.txt`; add new suite names to
`XPLOGIN_TEST_SUITES` there so CTest reports them individually.

---

## Part 2 - Manual testing in a VM

### 2.1 Build the VM

**Hyper-V** (elevated PowerShell on the host): a Generation 2 VM, 4 GB RAM,
2 CPUs, a 64 GB disk, Secure Boot on, the Windows ISO attached.

```powershell
New-VM -Name XPLogin10-Test -Generation 2 -MemoryStartupBytes 4GB `
       -NewVHDPath D:\vm\XPLogin10-Test.vhdx -NewVHDSizeBytes 64GB
Set-VMProcessor -VMName XPLogin10-Test -Count 2
Add-VMDvdDrive -VMName XPLogin10-Test -Path D:\iso\Win10_22H2.iso
Set-VMFirmware -VMName XPLogin10-Test `
       -FirstBootDevice (Get-VMDvdDrive -VMName XPLogin10-Test)
Start-VM -Name XPLogin10-Test
```

**VirtualBox**: new VM, *Windows 10 (64-bit)*, 4 GB RAM, 2 CPUs, 64 GB disk,
attach the ISO, install.

Either way, during setup:

- Create **at least three local accounts** so the welcome screen has something
  to show:
  - `Bill` - a normal account **with** a password
  - `Alice` - a normal account with a **different** password
  - `Kiosk` - an account with **no** password
- Optionally `net user Retired Passw0rd! /add` then
  `net user Retired /active:no` to have a disabled account.
- Turn off automatic logon if setup enabled it.

Then, **before installing anything**:

```powershell
# Hyper-V, on the host
Checkpoint-VM -Name XPLogin10-Test -SnapshotName 'clean-before-xplogin'
```

VirtualBox: *Machine → Take Snapshot*, name it `clean-before-xplogin`.

**Restoring that checkpoint is how you undo every test below.** Know the command
before you need it:

```powershell
Restore-VMCheckpoint -VMName XPLogin10-Test -Name 'clean-before-xplogin' -Confirm:$false
```

### 2.2 Get the setup into the VM

Copy a single file - `XPLogin10-Setup.exe` - into the VM. Everything else is
inside it, and there is deliberately nothing else to copy: the product is the
setup, the provider, the watchdog and the recovery CLI, and no preview or
development build ships beside them.

Before installing anything, enable a second way in so a broken logon screen is
an inconvenience rather than a reinstall - from an elevated prompt in the VM:

```powershell
# Confirm Safe Mode is reachable from the boot menu
bcdedit /set {bootmgr} displaybootmenu yes
bcdedit /set {bootmgr} timeout 5

# Confirm System Protection is on, so the installer's restore point works
Enable-ComputerRestore -Drive "C:\"
Get-ComputerRestorePoint | Format-Table SequenceNumber, Description, CreationTime
```

---

### Checklist A - the dry run

Unpack the setup's payload once (`XPLogin10-Setup.exe /dir C:\xp10-dry` then
cancel at the confirmation is *not* enough - nothing is written until you say
yes), or just copy `xplogin-install.exe` out of an installed folder. This
checklist proves the plan is sound before anything is registered.

| # | Step | Expected |
|---|---|---|
| A1 | `.\xplogin-install.exe /preview` | Prints the full registry plan; `Plan validation: passed` |
| A2 | `reg query "HKLM\SOFTWARE\XPLogin10"` | Not found - `/preview` changed nothing |
| A3 | `XPLogin10-Setup.exe`, read the summary, answer `n` | The summary names the install root, whether the Windows tiles are replaced, the restore point and the service; then "Cancelled. Nothing was changed." |
| A4 | `reg query "HKLM\SOFTWARE\XPLogin10"` again | Still not found |

**If A1–A4 do not all pass, stop.** Nothing below will work.

---

### Checklist B - the installer

Run `XPLogin10-Setup.exe` from the VM (not from a network share - the setup
copies itself into the install directory and needs to be readable).

| # | Step | Expected |
|---|---|---|
| B1 | Double-click `XPLogin10-Setup.exe` | A UAC prompt appears **immediately**, not part-way through |
| B2 | Read the summary, answer `n` | "Cancelled. Nothing was changed." |
| B3 | `reg query "HKLM\SOFTWARE\XPLogin10"` | Not found - declining really changed nothing |
| B4 | Run again, answer `y` | Six numbered steps, each `ok` |
| B5 | Watch step 2 | `restore point #NNN created` |
| B6 | `Get-ComputerRestorePoint` on the host VM | A point named **Before installing XPLogin10**, type `APPLICATION_INSTALL` |
| B7 | `dir "C:\Program Files\XPLogin10"` | Provider, watchdog, both `.ini`, `xplogin-install.exe`, **and `XPLogin10-Setup.exe`**. Nothing else |
| B8 | `dir "C:\Program Files\XPLogin10\XPLogin-Recovery.reg"` | Exists. **Copy it outside the VM now.** |
| B9 | Settings → Apps → Installed apps | **XPLogin10 - Windows XP logon screen**, version 1.0.0, with an Uninstall button and no Modify option |
| B10 | `sc query XPLoginWatchdog` | `RUNNING` |
| B11 | <kbd>Win</kbd>+<kbd>L</kbd> | The XP welcome screen |

**First-install safety net.** For everything below, prefer:

```
XPLogin10-Setup.exe /nofilter
```

which keeps the Windows tile reachable alongside the XP screen. Re-run without
it before Checklist G.

If B11 shows the Windows screen instead: check `XPLogin.log` in the install
folder, and confirm the DLL architecture matches the OS
(`dumpbin /headers XPLoginProvider.dll`).

### Checklist C - appearance

Compare against a real XP screenshot at 1024×768 if you have one.

| # | Check | Expected |
|---|---|---|
| C1 | Three horizontal bands | Dark blue header, lighter blue centre, dark blue footer |
| C2 | The dividers | XP's own 800px sweeps, stretched: a pale highlight under the header, an **orange** rule along the top of the footer |
| C3 | Vertical rule down the centre | Present in the tile list, fades away in the password view |
| C4 | Left column | "To begin, click your user name", right-aligned |
| C5 | Tiles | 48×48 picture inside a 58×58 plate with a soft drop shadow, name in bold to the right. The picture must be **visible**, not covered by the plate |
| C6 | Hover a tile | That tile stays solid and **the others fade back** to about a third opacity; the hovered plate turns gold |
| C6b | Move the pointer off the list | All tiles return to solid |
| C7 | Bottom left | XP's red power icon + "Turn off computer", both **below** the orange rule with clear space above and below |
| C7b | Bottom left, further right | Underlined "Other ways to log on" (absent if `signinoptions = replaceeverything`) |
| C8 | Bottom right | The two-line "After you log on…" hint, inside the band and **not touching the orange rule** |
| C8b | The logo | Microsoft's own 137×86 bitmap in the left column - flag, "Microsoft Windows", orange "xp" - right-aligned 20px short of the centre rule |
| C9 | Select a user | Tile slides *left and down*, easing out - not linear, not instant |
| C10 | Password view | White box, green arrow to its right, blue "?" beyond it, green back arrow to the left |
| C11 | Type | Bullets appear, caret blinks |
| C12 | Turn Caps Lock on | "Caps Lock is on" appears |
| C13 | Change the VM to 1920×1080 and lock again | Everything scales, stays centred, nothing clipped, bands still reach both edges |
| C14 | Change to 1280×1024 | Same |

---

### Checklist D - authentication

| # | Step | Expected |
|---|---|---|
| D1 | Right password | Signs in; **the XP logon sound plays** (installed by default) |
| D2 | Wrong password | Red XP error, shake, box cleared, tile still selected |
| D3 | Retype correctly after a failure | Signs in |
| D4 | `Kiosk` (no password), press Enter with an empty box | Signs in |
| D5 | Wrong password 5× on a domain account with a lockout policy | Windows locks the account; the screen says so, not "wrong password" |
| D6 | Click the disabled `Retired` tile | Refuses to open the password box, shows the disabled message |
| D7 | Back arrow from the password box | Returns to the tile list; the typed password is gone |
| D8 | Select a different tile after typing | Password box is empty, not carried over |
| D9 | `net user testuser P@ss1 /add`, then lock | `testuser` appears on the welcome screen |
| D10 | `net user testuser /delete`, then lock | It is gone |
| D11 | Sign in and watch the screen between Enter and the desktop | The tile and the password box go; a large italic "welcome" appears where the logo was. Never a frozen password box |
| D12 | Sign in, then look at the desktop | **One** Explorer: a taskbar and desktop icons, and no My Computer window |
| D13 | At the sign-in screen, type your password on the number pad | The digits arrive. `reg query "HKU\.DEFAULT\Control Panel\Keyboard" /v InitialKeyboardIndicators` reads `2` |
| D14 | Install with `/nonumlock`, lock, check the same value | Absent, and the number pad is whatever Windows left it as |

**One account with no password.** Needs its own VM state: remove every account
but one and clear its password (`net user Kiosk ""`).

| # | Step | Expected |
|---|---|---|
| D15 | Reboot | XP's **welcome** message, on the XP screen, for the whole of the sign-in. **Never** a one-tile account list, not even for a frame |
| D16 | `XPLogin.log` | `one account and no password: signing in without asking` |
| D17 | Add a second account, reboot | The ordinary account list is back |
| D18 | Back to one account, give it a password, reboot | The ordinary one-tile list, waiting |
| D19 | `autologonblankpassword = false` in `XPLogin.ini`, reboot | The one-tile list, waiting, even with no password |
| D20 | With one passwordless account: sign in, then <kbd>Win</kbd>+<kbd>L</kbd> | The unlock screen appears and stays. A lock that let go of itself would not be a lock |

**Microsoft accounts.** Untested territory - the automated suite covers the
classification and the qualified name, nothing has covered a real sign-in. Do
this on a VM installed with a Microsoft account, and install with `/nofilter`.

| # | Step | Expected |
|---|---|---|
| D21 | Install with `/nofilter`, lock | The XP screen lists the Microsoft account, by its display name |
| D22 | Type the Microsoft account password, Enter | Signs in. If it is refused, check D24 before anything else |
| D23 | `XPLogin.log` | The account's domain is `MicrosoftAccount`, and the name handed to LSA is `MicrosoftAccount\<email>` - the email alone authenticates nothing |
| D24 | Settings > Accounts > Sign-in options | If **"only allow Windows Hello sign-in for Microsoft accounts"** is **on**, Windows has disabled password sign-in and no credential provider can sign in with one. **Other ways to log on** > PIN is the way in, and the XP password box cannot work until that setting is off |
| D25 | `showdomain = false` in `XPLogin.ini`, lock | The Microsoft account is **still listed**. It is not a domain account, and hiding it would leave a machine with no tiles |
| D26 | `showmicrosoftaccounts = false`, lock | Now it is hidden. On a machine with no other account the filter gives up rather than lock you out - check the Windows tiles are back |

---

### Checklist D2 - PIN and Windows Hello

Skip this only if no account on the machine has a PIN. If one does, this is
the section that decides whether the install is safe to keep.

**Set up first:** on the test VM, Settings → Accounts → Sign-in options →
PIN → Add, and set one for your test account.

| # | Step | Expected |
|---|---|---|
| P1 | Lock (<kbd>Win</kbd>+<kbd>L</kbd>) | The XP screen appears as usual |
| P2 | Click **Other ways to log on** | The XP screen disappears and Windows' own tile strip is visible, with a PIN tile |
| P3 | Sign in with the PIN | Works, lands on the desktop |
| P4 | Lock again, press <kbd>Esc</kbd> at the account list | Same as P2 - this is the keyboard route |
| P5 | From the Windows tiles, pick the XPLogin tile | The XP screen comes straight back |
| P6 | With the XP screen up, look for a Windows **password** tile | There is none - that is the one thing the filter replaces |
| P7 | Check `XPLogin.log` after a lock | A "credential provider filter" block listing every CLSID with keep/hide and the reason |
| P8 | Set `signinoptions = replaceeverything`, lock | Only the XP screen; "Other ways to log on" is gone. **Verify you can still sign in with a password before rebooting.** |
| P9 | Put it back to `keepalternatives` | P2 works again |

> If an account has a PIN and **no** usable password, P8 will lock it out.
> That is why `keepalternatives` is the default and why the setup prints the
> PIN line before it asks to install.

---

### Checklist E - lock and session handling

| # | Step | Expected |
|---|---|---|
| E1 | Open Notepad, <kbd>Win</kbd>+<kbd>L</kbd>, unlock | Notepad is still open, same window position |
| E2 | Check Task Manager → Details | **One** `explorer.exe` for your session, not two |
| E3 | Start menu → Switch user | XP screen with all tiles |
| E4 | Sign in as Alice | New desktop; Bill's session still exists (Task Manager → Users) |
| E5 | Switch back to Bill | His Notepad is still there - session reconnected, not recreated |
| E6 | Sign out fully, then sign in again | Fresh session; a new `explorer.exe` |
| E7 | Let the screensaver lock the machine | XP screen, only the locked user's tile, no back arrow |

---

### Checklist F - shutdown

| # | Step | Expected |
|---|---|---|
| F1 | Click "Turn off computer" | XP modal: Stand By, Turn Off, Restart, and Cancel |
| F1b | Hover each orb, then press one | XP's own dialog: a 313×198 panel, amber/red/green orbs out of `msgina.dll` that brighten under the pointer and darken when pressed |
| F2 | Click Cancel | Returns to exactly where you were - tile list *or* password box |
| F3 | Click outside the dialog | Same as Cancel |
| F4 | Press Escape | Same as Cancel |
| F5 | Restart | The VM restarts and comes back to the XP screen |
| F6 | Turn Off | The VM powers off |
| F7 | `reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System" /v ShutdownWithoutLogon /t REG_DWORD /d 0 /f`, lock | The power button is disabled |
| F8 | Click **Turn Off** from the sign-in screen and watch | The account list and the logo disappear; "Windows is shutting down..." in large italic Arial where the logo was, bands still in place |
| F9 | Same with **Stand By** | "Preparing to stand by..." - lower case "stand by", which is how `winlogon.exe` spells it |

---

### Checklist G - the safety net itself

This is the most important section. **Take a second checkpoint first** so you
can repeat it:

```powershell
Checkpoint-VM -Name XPLogin10-Test -SnapshotName 'installed-working'
```

Now enable the filter for real:

```powershell
.\xplogin-install.exe /install      # no /nofilter this time
```

| # | Step | Expected |
|---|---|---|
| G1 | <kbd>Win</kbd>+<kbd>L</kbd> | XP screen only - **no** Windows tile anywhere |
| G2 | Sign in | Works |
| G3 | Simulate a crash loop: `reg add "HKLM\SOFTWARE\XPLogin10\Health" /v StartAttempts /t REG_DWORD /d 2 /f`, then lock | XP screen **and** the Windows tile - it unfiltered itself |
| G4 | Set `StartAttempts` to `3`, lock | The plain Windows logon screen - the provider stood down |
| G5 | Sign in normally, then `/status` | strikes `0`; next logon `run normally` |
| G6 | Reboot into Safe Mode | The Windows logon screen, never the XP one |
| G7 | From Safe Mode: `.\xplogin-install.exe /status` | next logon `stand down` |
| G8 | `.\xplogin-install.exe /disable`, reboot normally | The Windows logon screen |
| G9 | `.\xplogin-install.exe /enable`, lock | The XP screen is back |
| G10 | `sc query XPLoginWatchdog` | `RUNNING` |
| G11 | `.\XPLoginWatchdog.exe /console` after setting `StartAttempts` to 4 | Prints that it disabled the provider; the filter key is gone |

**G4 and G6 are the tests that matter.** If either fails, do not install this
anywhere else.

---

### Checklist H - offline recovery

Restore `installed-working`, then deliberately break it:

```powershell
# Point the registration at a DLL that does not exist.
reg add "HKCR\CLSID\{6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}\InprocServer32" /ve /t REG_SZ /d "C:\nope.dll" /f
```

| # | Step | Expected |
|---|---|---|
| H1 | Reboot | Windows cannot load the provider; you get *some* usable logon screen (the filter's own guard restores the Windows tiles when our provider is missing from the list) |
| H2 | Now also make the filter succeed but the provider fail, and reboot three times without signing in | By the third boot the provider has stood down on its own |
| H3 | Boot the VM from the Windows ISO → Repair → Command Prompt, run the `reg load` / `reg delete` / `reg unload` sequence from README.md → Recovery | Next boot shows the Windows logon screen |
| H4 | Restore `installed-working` | Back to a working XP screen |

---

### Checklist I - uninstall

Do this three times, once per route, restoring the `installed-working`
checkpoint in between.

**I-a. From Settings**

| # | Step | Expected |
|---|---|---|
| I1 | Settings → Apps → Installed apps → XPLogin10 → Uninstall | The setup opens with a confirmation |
| I2 | Confirm | Steps run, each `ok` |
| I3 | <kbd>Win</kbd>+<kbd>L</kbd> | The normal Windows sign-in screen |

**I-b. From the command line**

```
"C:\Program Files\XPLogin10\XPLogin10-Setup.exe" /uninstall /silent
```

**I-c. From the original download** - the copy outside the install folder must
find the installation through the registry, not from its own path.

**Then, whichever route, verify nothing is left:**

| # | Check | Expected |
|---|---|---|
| I4 | `reg query "HKLM\SOFTWARE\XPLogin10"` | Not found |
| I5 | `reg query "HKCR\CLSID\{6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}"` | Not found |
| I6 | `reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\XPLogin10"` | Not found |
| I7 | `sc query XPLoginWatchdog` | Does not exist |
| I8 | Settings → Apps | XPLogin10 is gone from the list |
| I9 | `dir "C:\Program Files\XPLogin10"` | Gone, or only `XPLogin10-Setup.exe` pending a restart |
| I10 | Restart, then check again | The folder is gone completely |
| I11 | `reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon"` | `Shell`, `Userinit` and everything else **intact** |
| I12 | `reg query "HKU\.DEFAULT\Control Panel\Keyboard" /v InitialKeyboardIndicators` | Gone - the Num Lock value is removed too |
| I13 | Reboot | Normal |

**I-d. Uninstalling a half-installed machine.** Restore `clean-before-xplogin`,
copy the files in by hand without registering anything, then run
`XPLogin10-Setup.exe /uninstall`. It must complete without errors - every
removal step is optional by design.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Windows logon screen after `/install` | Architecture mismatch (32-bit DLL, 64-bit OS), or the DLL path in the registry is wrong. Check `XPLogin.log`. |
| Black screen at logon | The window was created but painting failed. Reboot twice - it will unfilter itself. Then read `XPLogin.log`. |
| XP screen appears, Enter does nothing | `GetSerialization` is returning "not finished". Almost always the Negotiate package lookup failing; check the log for `LsaLookupAuthenticationPackage`. |
| Right password rejected | The packed blob is malformed. Run `--filter CredentialPacker.` - and if those pass on Windows, the SDK-struct assertion would have caught a layout drift. |
| No sound | The `.wav` paths in `[sounds]` are empty or point at missing files. See `tools/Import-XPSounds.ps1`. |
| Fonts look wrong | Franklin Gothic Medium is not installed; the fallback chain used Segoe UI. Install the font or change `[fonts] heading`. |
| No restore point was made | System Protection is off for `C:`, or Windows made one in the last 24 hours. `/forcerestorepoint` overrides the second; the first needs System Properties → System Protection. |
| Uninstall leaves the setup exe behind | Expected: it is running from the folder it is emptying. It is scheduled for delete-on-reboot. |
| "not a safe place to install to" | `/dir` pointed at a drive root, `C:\Windows`, or a bare `Program Files`. Pick a folder the installer can own outright. |

Logs: `XPLogin.log` next to the DLL, `XPLoginWatchdog.log` alongside it. Set
`[general] loglevel = 3` for the verbose version.

---

## Before calling it done

- [ ] `ctest --output-on-failure` - all green
- [ ] Checklists A through I pass on a clean VM
- [ ] G4 (self-disable) and G6 (Safe Mode) verified **specifically**
- [ ] Verified at 1024×768, 1280×1024 and 1920×1080
- [ ] Verified on Windows 10 **and** Windows 11
- [ ] Verified with a domain-joined VM if you will deploy to one
- [ ] A System Restore point really appears in `Get-ComputerRestorePoint` (B6)
- [ ] All three uninstall routes work (I-a, I-b, I-c) and leave nothing behind (I4-I12)
- [ ] Restoring the restore point from WinRE brings back a working machine
