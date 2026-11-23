# LightTranslate

A small tray app for Windows 11: select text in any application, press a hotkey
(default **Win + Alt + T**), and a translation pops up next to the caret.

- Single `.exe` (~0.5 MB), no runtime redistributable. Installer or portable zip.
- Providers: **DeepL** and **Google Cloud Translation (Basic, v2)** — bring your own API key.
- Idle cost: three sleeping threads, no timers, no polling.

## Install

Download from [Releases](https://github.com/LoveIiei/LightTranslator/releases):

- **`LightTranslate-<version>-setup.exe`** — installs for the current user only (no admin prompt)
  into `%LOCALAPPDATA%\Programs\LightTranslate`, adds a Start menu entry and optionally starts
  with Windows. Running a newer setup upgrades in place and keeps your settings.
- **`LightTranslate-<version>-portable-x64.zip`** — just the exe; run it from anywhere.

The binaries are not code-signed, so SmartScreen may show "Windows protected your PC" on first
run: click *More info → Run anyway*. `SHA256SUMS.txt` lists the checksums of the release files.

## Build

Requirements: Windows 11 (22H2+), Visual Studio 2022 with the C++ workload (CMake and Ninja are included).

From an *x64 Native Tools Command Prompt for VS 2022*:

```bat
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Output: `build\release\LightTranslate.exe`. WIL, nlohmann/json and doctest are fetched (pinned tags) on the first configure.

`tools\make-icons.ps1` regenerates the `.ico` files in `res\`.

### Release packages

`powershell -ExecutionPolicy Bypass -File tools\build-release.ps1` (any PowerShell window; needs
[Inno Setup 6](https://jrsoftware.org/isinfo.php), e.g. `winget install JRSoftware.InnoSetup`)
builds into `build\dist`, runs the tests and writes the installer, portable zip and
`SHA256SUMS.txt` to `dist\`. The installer script is `installer\LightTranslate.iss`.

To publish a GitHub release: bump `VERSION` in `project()` in `CMakeLists.txt` (it feeds the exe's
version resource and the installer), commit, then push a matching tag:

```bat
git tag v1.0.1
git push origin v1.0.1
```

`.github/workflows/release.yml` then builds, tests and creates the release with all three files attached.

### Smart App Control / SmartScreen

The exe is unsigned. An unsigned app that registers a global hotkey, injects Ctrl+C and reads the
clipboard looks keylogger-ish to reputation heuristics, so **Smart App Control** (if it is on) blocks
it — including locally built copies and the unit-test exe — and SmartScreen may warn on other
machines. Options: code-sign the binaries with a certificate your machine trusts
(`signtool sign /fd SHA256 /a build\release\LightTranslate.exe`), or build/run on a machine where
Smart App Control is off or in evaluation mode.

## Getting an API key

**DeepL** — create an account at <https://www.deepl.com/pro-api>, then copy the key from
*Account → API keys*. Free-plan keys end in `:fx`; the app picks the right endpoint
(`api-free.deepl.com` vs `api.deepl.com`) automatically. To override, set the registry value
`DeepLPlan` to `free` or `pro` (default `auto`).

**Google Cloud Translation** — in the Google Cloud Console: enable the *Cloud Translation API*,
then *APIs & Services → Credentials → Create credentials → API key*.
**Restrict the key to the Cloud Translation API** (*API restrictions*) so that a leaked key cannot
be used for anything else. Only the Basic (v2) API accepts API keys, so that is what the app uses.

Paste the key in **Settings** (tray icon → *Settings…*), press **Test**, then **Save**.

## Using it

| Action | How |
|---|---|
| Translate the selection | Win + Alt + T (configurable) |
| Close the popup | Esc, click elsewhere, or the hotkey again while the popup has focus |
| Copy | Ctrl + C or **Copy** (the selection, or everything if nothing is selected) |
| Change target language | click the target language in the popup header |
| Swap languages | click ⇄ at any time (a shown translation becomes the new source text; a fixed language pair is remembered in its new direction) |
| Edit the source text | type in the box at the top; it re-translates after a short pause |
| Keep the popup open | **Pin** (the position is remembered) |
| Pause the hotkey / autostart | tray icon menu |

## Privacy

- Selected text leaves your machine **only to the provider you chose**, over HTTPS (TLS 1.2/1.3,
  certificate checks always on). Nothing else is contacted.
- Translations are kept in RAM only (an LRU cache of up to 256 entries / 2 MB, cleared on quit).
  Nothing is written to disk except settings (registry) and API keys (Credential Manager).
- The clipboard is touched only during a hotkey-triggered capture, and only if UI Automation
  could not read the selection. There is no clipboard listener, monitor or history at any other time.
- Password fields are refused, as is clipboard content marked
  `ExcludeClipboardContentFromMonitorProcessing` (what password managers set).

### Known limitations

- **Win+V history caveat.** In the clipboard-fallback path the *source application* writes the
  selection to the clipboard, so it can appear in Win+V clipboard history (the restore afterwards is
  tagged so that it does not create a second entry). If that matters to you, turn off
  *Allow clipboard fallback* in Settings; apps that cannot be read through UI Automation will then
  report "No selected text found".
- **Clipboard restore is best effort.** Text, images (DIB/PNG), file lists, HTML and RTF are
  restored (up to ~16 MB; above that only text). Exotic formats (OLE objects, delay-rendered private
  formats) may not survive a fallback capture.
- **Elevated applications** cannot be read by a non-elevated process (UIPI). You get a message, not a hang.
- **API keys** are stored in Windows Credential Manager. This keeps them out of config files,
  the registry, backups and screenshots. It does *not* protect against malware running as your user.
- The DeepL language table was written from DeepL's documentation; if DeepL rejects a language
  pair you get a "rejected the request" error rather than a wrong translation.
- No offline translation, OCR or auto-update. The unofficial free Google endpoint is deliberately not used.

## Settings reference (`HKCU\Software\LightTranslate`)

| Value | Meaning |
|---|---|
| `HotkeyModifiers`, `HotkeyVk` | hotkey (`MOD_*` flags, virtual key) |
| `Provider` | `deepl` or `google` |
| `SourceLang`, `TargetLang` | `auto` or a language code such as `de`, `en-US`, `pt-BR` |
| `AllowClipboardFallback` | 1 = allow the Ctrl+C fallback |
| `DeepLPlan` | `auto` / `free` / `pro` |
| `MaxChars` | capture limit, default 5000 (100–50000) |
| `UseBackdrop` | 0 = solid popup background instead of Acrylic |
| `PopupPinned`, `HasPinPosition`, `PinX`, `PinY` | pinned popup state |

## Uninstall

**Installed:** *Settings → Apps → Installed apps → LightTranslate → Uninstall*. It closes the app,
removes autostart, and asks whether to also delete your settings and saved API keys.

**Portable:**

1. Quit from the tray icon and delete `LightTranslate.exe`.
2. Delete the registry key `HKCU\Software\LightTranslate`
   (and the `LightTranslate` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` if autostart was on).
3. In *Credential Manager → Windows Credentials → Generic Credentials*, remove
   `LightTranslate/deepl` and `LightTranslate/google`.

## Layout

```
src/app        App (message routing), Settings (registry), Messages
src/core       Encoding, HotkeyManager, TextCapture, ClipboardBackup, TranslatorClient, LruCache, SecretStore
src/providers  ITranslationProvider, ProviderRegistry, DeepL, Google, Languages
src/ui         TrayIcon, PopupWindow, PopupRenderer, SettingsDialog, Theme
tests          doctest unit tests + JSON fixtures
```

Adding a provider: one new `.cpp` implementing `ITranslationProvider`, plus its factory in
`ProviderRegistry.cpp` (and the file in `LT_PROVIDER_SOURCES` in `CMakeLists.txt`).
