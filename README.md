# XPLogin10

A Windows XP welcome screen for Windows 10 and 11 - not a screenshot and not a
wallpaper, but a real credential provider that replaces the logon UI and signs
you in through LSA exactly as the Windows screen does.

![The XP welcome screen](docs/preview/01-userlist.svg)

<sub>Not a mock-up and not a redraw. Every rectangle in that picture was
computed by the shipping `XpLayout::Compute()`; the logo, the divider sweeps,
the account plate, the password field and the buttons are the actual bitmaps
out of XP's `logonui.exe`, extracted by the build. More states in
[docs/preview](docs/preview).</sub>

> ### Tested configuration
> **Windows 10 21H2 x64, signing in with a local account.** That is the only
> configuration this has been run on end to end, and everything below that is
> stated as working was verified there.
>
> A machine that signs in with a **Microsoft account**, a domain account or an
> AzureAD work account is handled by the code and covered by tests, but has not
> been sat in front of. See
> [Microsoft accounts and domain accounts](#microsoft-accounts-and-domain-accounts)
> before installing on one, and use `/nofilter` the first time.

---

## What this actually is

Windows has not had a replaceable logon UI since XP: GINA was removed in Vista.
The supported extension point is a **credential provider**, a COM object that
`LogonUI.exe` loads on the secure Winlogon desktop. XPLogin10 is one of those,
plus a filter that hides the stock Windows tiles, plus a full-screen window that
draws the XP welcome screen on top.

The important consequence: **XPLogin10 never checks a password itself.** It packs
what you typed into a `KERB_INTERACTIVE_UNLOCK_LOGON` buffer and hands it to
LogonUI. LSA validates it, builds the token, creates the session, loads the
profile and starts `explorer.exe`. That is why signing in through the XP screen
lands you on a completely normal Windows desktop, and why domain accounts,
account lockout, logon hours and auditing all keep working.

| Piece | What it is | Where |
|---|---|---|
| `XPLoginProvider.dll` | `ICredentialProvider` + `ICredentialProviderFilter` + the XP window | `src/provider`, `src/ui` |
| `XPLoginWatchdog.exe` | session-0 service; crash-loop recovery | `src/service` |
| `XPLogin10-Setup.exe` | the installer: one file, payload embedded | `src/setup` |
| `xplogin-install.exe` | recovery CLI: registration only, the thing you run from Safe Mode | `src/installer` |
| `xplogin_core` | auth, state machine, accounts, health - no Windows | `src/core` |
| `xplogin_assets` | PE resource reader, BMP/RLE decoder, asset pack, PNG writer | `src/assets` |
| `xplogin_tests` | 435 automated tests | `tests` |

Those four binaries are the whole product. `XPLogin10-Setup.exe` deploys
nothing that is not on that list.

### Scope: the sign-in screen, and only the sign-in screen

This replaces the screen you **sign in on**: the account list, the password
box, the "Turn off computer" dialog, the unlock screen after Win+L. It does not
replace the screens Windows draws while it is **signing out, restarting or
shutting down** - those stay Windows' own, with its spinner.

That is a deliberate limit rather than a missing feature, and
[Known limits](#known-limits) says exactly why it cannot be done from a
credential provider.

### Why native C++ rather than C#/WPF

A credential provider is loaded **into `LogonUI.exe`**, on the secure desktop.
Hosting the CLR there is unsupported and a reliable way to make a machine
unbootable-to-desktop. Everything that runs inside LogonUI is native C++ with
GDI+; the CLR never enters the picture.

The logic that does *not* need Windows - the state machine, the credential
packing, the account visibility rules, the layout maths, the crash-loop
recovery - lives in `src/core` and `src/ui` behind interfaces, and is compiled
and tested on any host. That is what makes a logon screen testable at all.

---

## Requirements

* Windows 10 1809+ or Windows 11, x64
* Visual Studio 2022 with **Desktop development with C++** and the Windows SDK
  (10.0.19041 or newer)
* CMake 3.16+
* **A virtual machine to test in.** See [TESTING.md](TESTING.md). Do not install
  this on a machine you need.

---

## Build

```powershell
git clone https://github.com/SubCoderHUN/XPLogin10.git
cd XPLogin10
.\tools\Build.ps1                       # Release / x64, runs the tests
```

Or by hand:

```powershell
cmake -S . -B build -A x64 -DXPLOGIN_BUILD_WIN32=ON
cmake --build build --config Release --parallel
cd build && ctest -C Release --output-on-failure
```

The architecture must match the OS. A 32-bit provider cannot be loaded by a
64-bit `LogonUI.exe`; it will simply never appear, with no error anywhere.

Everything links the **static** CRT (`/MT`), so nothing here needs a Visual
C++ redistributable on the target machine. That is not tidiness: a provider
DLL whose `MSVCP140.dll` is missing or a different build is one `LogonUI`
declines to load, silently, leaving the Windows logon screen and no
explanation - and an installer that needs a redistributable installed before
it will run is not an installer.

### Where the artwork comes from

The screen uses Microsoft's own bitmaps, and the build is what puts them there:

```
assets/system32/logonui.exe          reference binary, checked in
assets/system32/msgina.dll           the turn-off dialog's artwork
        |
        +-- tools/xp-extract         reads the PE resource tree; --print dumps
        |                            the DirectUI markup the layout came from
        |
        +-- tools/xp-bake  ------>   build/XPLogin.assets  (25 bitmaps)
                                             |
                                             +- embedded in XPLogin10-Setup.exe
                                             +- deployed beside the provider
```

`cmake/BakeAssets.cmake` runs `xp-bake` automatically if
`assets/system32/logonui.exe` is present. **If it is absent the build still
works** - no pack is produced, and `XpRenderer` draws the same screen from GDI+
primitives instead. Nothing about installing or signing in depends on the pack
existing.

Neither `xp-extract` nor `xp-bake` calls `LoadLibrary`: they parse the PE with
a bounds-checked byte reader (`src/assets/src/PeResources.cpp`), because handing
an untrusted binary to the loader to read a bitmap out of it is not a trade
worth making.

### Building just the core and the tests (any platform)

The portable half builds and tests on Linux and macOS too, which is how CI
checks it:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/bin/xplogin_tests
```

---

## Install

Download `XPLogin10-Setup.exe` and run it. That is the whole procedure - the
setup carries the provider, the watchdog, the config and the theme inside
itself, so there is nothing to unpack and nothing else to download.

```
XPLogin10 setup
------------------------------------------------------------
Install to      : C:\Program Files\XPLogin10
Windows tiles   : password tiles replaced by the XP screen
PIN and Hello   : still available - "Other ways to log on" in the bottom left,
                  or press Escape at the account list
Restore point   : yes
Watchdog service: yes

This replaces the Windows sign-in screen. If something goes wrong,
Safe Mode always shows the normal Windows screen, and the installer
leaves an XPLogin-Recovery.reg next to the program files.

Install XPLogin10? [y/N]
```

It asks for elevation on the double-click (the manifest requests it up front,
so the UAC prompt never lands halfway through), then:

1. **Takes a System Restore point** - `Before installing XPLogin10`, an
   Application Install checkpoint, *before anything is changed*. If System
   Protection is off, or Windows already made one today, it says so and carries
   on; it is not worth refusing to install over.
2. Writes `XPLogin-Recovery.reg` - before the registry is touched, because
   after that point the machine may not boot far enough to write it.
3. Copies the files to `C:\Program Files\XPLogin10`.
4. Registers the credential provider - *after* the DLL is on disk, never
   before.
5. Installs the watchdog service.
6. Adds the **Programs and Features** entry.

Nothing outside that folder and those registry values is touched. No system
binary is modified, no certificate is added to any store, and no driver is
installed.

### What about a PIN, Windows Hello, or a fingerprint?

They keep working. The XP screen replaces the **password** tiles - the ones it
is imitating - and leaves PIN, Hello, fingerprint, smartcard and any provider
it does not recognise alone. **Other ways to log on** in the bottom left, or
<kbd>Esc</kbd> at the account list, steps the XP screen aside so those tiles
are reachable; picking the XPLogin tile again brings it straight back.

This is not politeness, it is the difference between a theme and a lockout. A
PIN is not a password: it unlocks a TPM-held key, and there is no way to feed
one through the XP password box, because LSA has no secret to compare. On an
account that signs in *only* with a PIN - a Microsoft account, a passwordless
work account - hiding that tile leaves no way in at all.

If every account on the machine has a real password and you want the fully
authentic screen with nothing else on it, set `signinoptions = replaceeverything`
in `XPLogin.ini`. Test signing in with a password *before* you do.

Whatever that setting says, three things always hold and are covered by tests:
XPLogin never hides its own tile, it abandons filtering entirely if the plan
would leave no way to sign in, and Safe Mode bypasses the whole thing.

### One account with no password

Windows signs such a machine in without asking, and so did XP. The screen
agrees with that rather than arguing: it reports the credential as ready and
shows XP's **welcome** message for the whole of it. What it does not do is put
up an account list with a single tile on it and have something click past that
tile a moment later, which is a flash of a screen nobody was meant to interact
with.

The rule is deliberately narrow, and every part of it is about the account list:

* exactly one account - two mean a choice, and a choice means a screen;
* that account has no password;
* it is not disabled, locked out or password-expired, because LSA would refuse
  the blank password and leave the screen on an error with no tile behind it;
* the sign-in screen only. An unlock stays a screen even for a passwordless
  account: a Win+L that let go of itself the moment it was pressed would be a
  lock that does not lock.

`[users] autologonblankpassword = false` in `XPLogin.ini` turns it off and
gives the one-tile screen that waits.

### Microsoft accounts and domain accounts

**Short answer: it should work, and it has not been tested.** Here is exactly
what is and is not known, because the difference matters on a machine with no
local account to fall back on.

**What the code does.** A Microsoft account is a local account underneath: a
local profile with a local SID, which Windows names
`MicrosoftAccount\someone@example.com`. Three things follow, and all three are
implemented and unit tested:

* The account list comes from `ICredentialProviderUserArray` - Windows' own
  list, the same one its sign-in screen uses - so the tile appears without this
  project having to enumerate anything itself.
* That qualified name is carried to LSA **unchanged**, domain half included.
  The email address on its own authenticates nothing; `MicrosoftAccount` is
  part of the identity, not decoration.
* It is classified as a Microsoft account rather than a domain one, so
  `showdomain = false` cannot hide it. On a machine set up with a Microsoft
  account and nothing else, that would have hidden the only way in.

The same applies to a domain or AzureAD account, with `CONTOSO\alice` or
`AzureAD\someone@company.com` in place of the first.

**The real risk, and it is not about this project.** Windows has a setting
called **"For improved security, only allow Windows Hello sign-in for Microsoft
accounts"**. When it is on - and it is on by default on many installs - Windows
**disables password sign-in for Microsoft accounts entirely**. No credential
provider can work around that, including this one and including Microsoft's:
there is no password for LSA to check. The way in is then a PIN, through
**Other ways to log on**, which the default `keepalternatives` setting keeps
reachable from the bottom left of the XP screen.

So on a Microsoft-account machine:

```
XPLogin10-Setup.exe /nofilter
```

Install with `/nofilter` first. It keeps the Windows tiles beside the XP screen,
so if the password box turns out to be a dead end the PIN tile is one click
away rather than one reboot away. Turn the setting off in Settings > Accounts >
Sign-in options if you want the password path, then re-run the setup without
`/nofilter`.

### Num Lock

On by default, because XP's screen had it and because there is no way to turn it
on from the Windows sign-in screen - so a PIN or a password typed on the number
pad silently does nothing. It is Windows' own mechanism
(`InitialKeyboardIndicators` under the default user profile, the hive Winlogon
reads before anybody has signed in), set to `2`, and removed again on uninstall.
`/nonumlock` leaves it alone.

### First install: leave yourself a way in

```
XPLogin10-Setup.exe /nofilter
```

`/nofilter` keeps the Windows tiles visible *alongside* the XP screen, so a
misbehaving welcome screen is an annoyance rather than a lockout. Re-run without
it once you are happy.

### Options

| Flag | Effect |
|---|---|
| `/silent` | no prompts, for deployment |
| `/dir <path>` | install somewhere other than Program Files |
| `/nofilter` | leave the Windows credential tiles visible |
| `/nolockscreen` | keep the Windows 10 lock screen image |
| `/nonumlock` | leave Num Lock alone at the sign-in screen |
| `/cad` | require <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>Del</kbd> first |
| `/noservice` | skip the watchdog service |
| `/norestorepoint` | skip the System Restore checkpoint |
| `/forcerestorepoint` | take one even if Windows made one in the last 24 h |

---

## Uninstall

Three ways, all equivalent:

* **Settings > Apps > Installed apps > XPLogin10 > Uninstall**
* **Control Panel > Programs and Features**
* `"C:\Program Files\XPLogin10\XPLogin10-Setup.exe" /uninstall`
  (add `/silent` for no prompts)

It stops the service, unregisters the provider *first*, and only then deletes
the files - a registration pointing at a missing DLL is exactly the state that
breaks logon. Anything still locked (the setup binary is running from the folder
it is emptying) is scheduled for removal on the next restart.

What is left behind afterwards: nothing. `HKLM\SOFTWARE\XPLogin10`, both CLSID
registrations, the filter, the Num Lock value, the service and the install
directory all go. The uninstaller never deletes a directory it did not create,
and never removes a whole Windows-owned key - only the individual values it
wrote. All of these rules are pinned by tests, the ordering ones included.

---

## Recovery - read this before installing

Five independent ways back, in the order you would reach for them.

**1. System Restore.** The setup takes a checkpoint before it changes anything.
Boot to WinRE (hold <kbd>Shift</kbd> while choosing Restart) > Troubleshoot >
Advanced options > System Restore, and pick *Before installing XPLogin10*.

**2. It disables itself.** The provider records a strike every time it starts and
clears them on any successful sign-in. Two strikes with no sign-in in between and
it stops hiding the Windows tiles; three and it refuses to load at all. A crash
loop therefore costs at most three reboots and fixes itself.

**3. Safe Mode.** The provider checks `SM_CLEANBOOT` and never loads there. Boot
into Safe Mode (hold <kbd>Shift</kbd> while choosing Restart > Troubleshoot >
Advanced options > Startup Settings), sign in normally, then:

```powershell
.\xplogin-install.exe /uninstall
```

**4. The recovery `.reg`.** The setup writes `XPLogin-Recovery.reg` next to the
binaries *before* it changes anything. From WinRE's command prompt:

```
reg load HKLM\OFFLINE C:\Windows\System32\config\SOFTWARE
reg delete "HKLM\OFFLINE\Microsoft\Windows\CurrentVersion\Authentication\Credential Provider Filters\{9C1D3E82-40B6-4F1D-8A57-2E9C6B04D311}" /f
reg delete "HKLM\OFFLINE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}" /f
reg unload HKLM\OFFLINE
```

Deleting the **filter** key alone is enough to bring the Windows tiles back.

**5. The VM checkpoint.** Which is why you are testing in a VM.

---

## Configuration

`XPLogin.ini` next to the DLL - see the file itself, every key is documented and
set to its default. Highlights:

```ini
[general]
filterotherproviders = true    ; false = Windows tiles stay visible too
maxfailedattempts = 0          ; back to the tile list after N misses; 0 = never

[users]
showadministrator = false      ; XP hid it unless it was the only account
hidden = kiosk, svc_backup

[safety]
unfilterthreshold = 2          ; strikes before the Windows tiles come back
bypassthreshold   = 3          ; strikes before we refuse to load
```

`HKLM\SOFTWARE\XPLogin10\Enabled` overrides the file, so the whole thing can be
turned off by policy.

### Appearance

`XPLogin.theme.ini` holds every colour, gradient stop, metric, font and string.
Metrics are in XP's 1024x768 design units and scale uniformly to the real
resolution. Nothing needs rebuilding: edit the file next to the DLL and lock the
screen.

Every default in it was read out of Microsoft's binaries rather than measured
off a screenshot, and the source is cited in the code beside each one -
`[UIFILE]` for the DirectUI markup in `logonui.exe`, `[DIALOG]` for the turn-off
dialog template in `msgina.dll`. `tools/xp-extract` is what prints them:

```powershell
.\build\bin\xp-extract .\assets\system32\logonui.exe --type UIFILE --print
```

### Avatars and sounds

Point `[ui] avatardirectory` at a folder of `<username>.png` (or `.bmp`) tile
pictures. Without one, tiles get XP's own default account picture.

The sounds are installed and on by default: XP's logon, logoff, error and
shutdown `.wav` files are embedded in the setup, deployed next to the DLL and
resolved automatically when `[sounds]` leaves the paths empty. Set
`enabled = false` for a silent screen, or point the paths at your own files. A
path that does not exist is skipped silently - nothing here can stop somebody
signing in.

Both the sounds and the artwork come from `assets/` at build time; see
[NOTICE.md](NOTICE.md) for where they come from and what that means if you
redistribute.

---

## Tests

```bash
cd build && ctest --output-on-failure
```

435 tests across 26 suites, all running without a logon session:

| Area | What is covered |
|---|---|
| `CredentialPacker` | the exact byte layout LSA expects, 32- and 64-bit, bounds checks |
| `Authentication` | right/wrong password, unknown user, disabled, locked out, lockout counting |
| `AuthStatusMapping` | every NTSTATUS to the XP message the user sees |
| `StateMachine` | lock, login, shutdown; every refused transition |
| `HealthMonitor` | the crash-loop recovery ladder, branch by branch |
| `RegistrationPlan` | every registry key, and the guard that rejects writes outside them |
| `Deployment` | the file plan, the payload manifest, and the guard that refuses `C:\Windows` as an install root |
| `SigninOptions` | which tiles the filter hides, the two ways it gives up rather than lock you out, and when a machine signs itself in |
| `ProgramsAndFeatures` | the uninstall entry, and that it is removed again |
| `SetupPlan` | the ordering rules: restore point first, files before registration, registry removed before files |
| `SystemRestore` | checkpoint created / rate limited / protection off / unsupported |
| `ShellStatus` | which XP status line each state shows |
| `PeResources` | the PE resource tree, with malformed and hostile inputs |
| `BmpDecoder` | BI_RGB / BI_BITFIELDS / RLE8 / RLE4, top-down and bottom-up |
| `AssetPack` / `Assets` | the pack format, and the real bitmaps decoded byte-for-byte |
| `PngWriter` | the deflate stream, round-tripped through an independent inflate |
| `Theme` / `Layout` / `HitTest` / `Animation` | geometry, scaling, overlap, clicks |
| `IniFile` / `AppConfig` | configuration parsing, including malformed input |
| `UserEnumeration` | dynamic account queries and XP's visibility rules |
| `SessionSimulation` | logon, Win+L, unlock, fast user switching, shell launch |
| `LogonFlow` | the whole flow end to end, including the LogonUI handshake |

Filter with `./build/bin/xplogin_tests --filter StateMachine.` and list with
`--list`.

Manual verification - the part that cannot be automated, because it needs a real
Winlogon desktop - is in **[TESTING.md](TESTING.md)**.

---

## How it fits together

```
       Winlogon --> LogonUI.exe          Service Control Manager
                        |  loads                    |  starts
                        v                           v
            +---------------------------+   +--------------------------+
            |    XPLoginProvider.dll    |   |   XPLoginWatchdog.exe    |
            |                           |   |  session 0               |
            |  XPProvider   XPFilter    |   |  crash-loop recovery     |
            |       |                   |   +--------------------------+
            |  XPCredential --> LSA     |  packs KERB_INTERACTIVE_UNLOCK_LOGON
            |       |                   |
            |  XpLogonWindow            |  full-screen window, Winlogon desktop
            |       |                   |
            |  XpRenderer (GDI+)        |  draws the XP screen
            +-------|-------------------+
                    | drives
            +-------v-------------------+
            |     LogonController       |  <- the tests drive this directly
            |  StateMachine . Directory |
            |  HealthMonitor            |
            +-------|-------------------+
                    | interfaces
    IAuthenticator . IUserEnumerator . ISessionManager . IPowerController
         |                                        |
    Win32 impls                              test fakes
```

`LogonController` is the same object in production and in the tests. Only what
is underneath it changes.

---

## Known limits

* **The sign-out, restart and shutdown screens stay Windows' own.** This is the
  one thing that was tried and abandoned rather than never attempted, so here is
  what was actually measured, in case somebody is tempted:
  * `LogonUI.exe` does not persist and is never sent `WM_QUERYENDSESSION`, so a
    credential provider cannot be running when the machine goes down.
  * A separate process in the session can be, and its window does reach the
    display - verified by reading the screen pixels back, not by asking the
    window manager. But csrss terminates every process in the session within
    about 200ms of the sign-out starting, including one started *after* the
    sweep begins, and then the session itself is destroyed. There is nothing
    left to draw from.
  * The last stretch is drawn by the boot-graphics path, outside the window
    manager entirely. No window can cover it at any z-order or band.

  Covering it would mean patching signed Windows binaries, which fails Secure
  Boot, is reverted by every cumulative update, and risks a machine that cannot
  sign in at all. That trade is not worth a picture.
* **CredUI is left alone.** The in-session credential prompt (UAC-style "enter
  your password" dialogs) keeps the Windows look. Replacing it with a
  full-screen XP window would be wrong, so `SetUsageScenario` returns
  `E_NOTIMPL` for `CPUS_CREDUI` and `CPUS_CHANGE_PASSWORD`.
* **Password hints.** XP's "?" button showed the hint. Windows 10 does not
  expose local-account hints to the logon desktop, so the button is drawn and
  does nothing rather than showing something invented.
* **Fast user switching** reconnects an existing session but does not draw XP's
  session-switch animation.
* **Secure Attention Sequence.** With `/cad`, Windows draws its own
  Ctrl+Alt+Del screen first; that one genuinely cannot be replaced, and should
  not be.
* **Code integrity.** This is a user-mode COM server, not a driver, so
  driver-signing rules do not apply - but a managed machine's WDAC policy may
  still require the DLL to be signed before LogonUI will load it.

---

## Licence and trademarks

The code is this project's. The *look* is Microsoft's: "Windows" and "Windows
XP" are their trademarks, and no Microsoft artwork, font or sound is included
here. See [NOTICE.md](NOTICE.md) for what that means in practice and how to
supply your own licensed assets.
