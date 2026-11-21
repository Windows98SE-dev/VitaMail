# VitaMail

A native PS Vita email client supporting **IMAP4rev1 over TLS** (IMAPS / STARTTLS) and **SMTP over TLS** (SMTPS / STARTTLS), built with VitaSDK, SDL2, FreeType2, and mbedTLS.

## Requirement: iTLS-Enso Certificate Store

VitaMail performs **strict TLS 1.2 certificate verification** (including certificate expiration checks against the Vita's RTC) using the console's root CA store at:

```
vs0:/data/external/cert/CA_LIST.cer
```

Stock PS Vita firmware ships with an outdated/expired CA list that fails against modern mail servers. Before using VitaMail:

1. Install **iTLS-Enso** (v3.2.1 or newer) on your PS Vita.
2. Open iTLS-Enso and select **"Install the full iTLS package"** (or **"Install the iTLS CA certs"**).
3. Reboot your console.
4. Ensure your PS Vita's date & time are accurate (via Settings -> Date & Time -> Set via Internet).

You can inspect the loaded CA certificate count at any time inside VitaMail by pressing **Select** (or tapping the `iTLS` status badge in the top-right corner).

## End-User Setup & Controls

- **Data directory:** `ux0:data/VitaMail/` (account configuration is saved to `ux0:data/VitaMail/accounts.cfg`).
- **Foreground-only:** PS Vita homebrew cannot run background services; mail syncs when you open the app or press **Triangle** to refresh.
- **App Passwords:** Providers like Gmail, iCloud, and Yahoo require an **App Password** instead of your main account password when 2FA is enabled. Built-in presets are included for Gmail, iCloud, Outlook, Yahoo, Fastmail, and Custom IMAP/SMTP.

### Controls
- **D-Pad / Touch:** Navigate folders, email list, and fields
- **Right Stick:** Scroll message body in Reader view
- **Cross (✕):** Open email / Edit field (Vita OS Keyboard) / Delete in Reader
- **Circle (○):** Back / Cancel
- **Square (□):** Compose new email (or Reply in Reader)
- **Triangle (△):** Refresh current folder (or Toggle Read/Unread in Reader)
- **L / R:** Switch folders
- **Start:** Account Settings
- **Select:** iTLS Certificate Store diagnostics

## Developer Build Instructions

### Build PS Vita VPK
Requires [VitaSDK](https://vitasdk.org/) with `sdl2` and `freetype` installed:

```bash
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Output: `build/VitaMail.vpk` (Title ID `VTML00001`).

### Build & Run Host Unit Tests
```bash
cmake -S . -B build_host -DCMAKE_BUILD_TYPE=Debug
cmake --build build_host -j
ctest --test-dir build_host --output-on-failure
```
