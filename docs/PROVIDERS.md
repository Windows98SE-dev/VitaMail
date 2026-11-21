# Email Provider Setup Guide

VitaMail connects to mail servers using standard **IMAP4rev1** and **SMTP** over **TLS 1.2**.  
Most major email providers **do not** allow your regular account password for IMAP/SMTP—you must generate an **App Password** (and enable IMAP if disabled).

---

## 1. Gmail (Google Account)

> **Why "nothing happens" or login fails with a regular Google password:**  
> Google permanently removed "Less Secure Apps" password access. Using your normal Google password will be rejected with `AUTHENTICATIONFAILED`. You **must** use a 16-character **App Password**.

### Step-by-Step Setup
1. **Enable 2-Step Verification** on your Google Account:
   - Go to [myaccount.google.com/security](https://myaccount.google.com/security) -> **2-Step Verification** -> turn it **ON**.
2. **Generate an App Password**:
   - Visit [myaccount.google.com/apppasswords](https://myaccount.google.com/apppasswords) (or search "App passwords" in Google Account settings).
   - Enter a name like `PS Vita` and click **Create**.
   - Copy the **16-character password** (e.g. `abcd efgh ijkl mnop`). You can enter it in VitaMail without spaces (`abcdefghijklmnop`).
3. **Verify IMAP is enabled in Gmail** (usually enabled by default):
   - Open Gmail on a browser -> **Settings (gear)** -> **See all settings** -> **Forwarding and POP/IMAP** -> ensure **Enable IMAP** is selected.
4. **VitaMail Settings** (`Gmail (App Password)` preset):
   - **Email:** `yourname@gmail.com`
   - **Password:** your 16-character App Password
   - **IMAP Host:** `imap.gmail.com` | **Port:** `993` | **Security:** `Implicit TLS (SSL)`
   - **SMTP Host:** `smtp.gmail.com` | **Port:** `465` | **Security:** `Implicit TLS (SSL)`

---

## 2. iCloud Mail (Apple ID)

> **Requirement:** Apple requires an **App-Specific Password** for third-party IMAP/SMTP clients.

### Step-by-Step Setup
1. Ensure **Two-Factor Authentication** is enabled on your Apple ID.
2. Sign in at [appleid.apple.com](https://appleid.apple.com/).
3. Go to **Sign-In and Security** -> **App-Specific Passwords** -> **Generate an app-specific password**.
4. Name it `VitaMail` and copy the generated `xxxx-xxxx-xxxx-xxxx` password.
5. **VitaMail Settings** (`iCloud Mail` preset):
   - **Email:** `yourname@icloud.com` (or `@me.com` / `@mac.com`)
   - **Password:** the App-Specific Password
   - **IMAP Host:** `imap.mail.me.com` | **Port:** `993` | **Security:** `Implicit TLS (SSL)`
   - **SMTP Host:** `smtp.mail.me.com` | **Port:** `587` | **Security:** `STARTTLS`

---

## 3. Outlook / Hotmail / Live (Microsoft)

> **Important Microsoft Limitation (Modern Auth):**  
> As of September 2024, Microsoft retired Basic Authentication (username + password / app password) for personal `@outlook.com`, `@hotmail.com`, and `@live.com` IMAP/SMTP accounts in favor of OAuth2.  
> Because VitaMail uses IMAP/SMTP password authentication, personal Microsoft accounts will fail authentication unless Microsoft's server accepts an app password on your specific tenant/account. Microsoft 365 Work/School accounts only work if your administrator explicitly allows Basic Auth for IMAP/SMTP.

### VitaMail Settings (`Outlook / Hotmail` preset)
- **Email:** `yourname@outlook.com`
- **Password:** App Password (if permitted by your account)
- **IMAP Host:** `outlook.office365.com` | **Port:** `993` | **Security:** `Implicit TLS (SSL)`
- **SMTP Host:** `smtp.office365.com` | **Port:** `587` | **Security:** `STARTTLS`

---

## 4. Yahoo Mail

> **Requirement:** Yahoo requires a generated **App Password**.

### Step-by-Step Setup
1. Go to [login.yahoo.com/account/security](https://login.yahoo.com/account/security).
2. Scroll to **Generate app password** (or **Manage app passwords**).
3. Select **Other App**, enter `VitaMail`, and click **Generate**.
4. **VitaMail Settings** (`Yahoo Mail` preset):
   - **Email:** `yourname@yahoo.com`
   - **Password:** the generated App Password
   - **IMAP Host:** `imap.mail.yahoo.com` | **Port:** `993` | **Security:** `Implicit TLS (SSL)`
   - **SMTP Host:** `smtp.mail.yahoo.com` | **Port:** `465` | **Security:** `Implicit TLS (SSL)`

---

## 5. Fastmail

> **Requirement:** Fastmail requires an **App Password** scoped for IMAP and SMTP.

### Step-by-Step Setup
1. Log in to Fastmail -> **Settings** -> **Privacy & Security** -> **Integrations** -> **App passwords**.
2. Click **New app password**, name it `VitaMail`, and enable **IMAP** and **SMTP** access.
3. **VitaMail Settings** (`Fastmail` preset):
   - **Email:** `yourname@fastmail.com` (or your custom domain)
   - **Password:** the generated App Password
   - **IMAP Host:** `imap.fastmail.com` | **Port:** `993` | **Security:** `Implicit TLS (SSL)`
   - **SMTP Host:** `smtp.fastmail.com` | **Port:** `465` | **Security:** `Implicit TLS (SSL)`

---

## 6. Custom IMAP / SMTP Providers

Select the **`Custom IMAP/SMTP`** preset in VitaMail Settings and enter your provider's details:

| Provider | IMAP Host | IMAP Port / Security | SMTP Host | SMTP Port / Security | Notes |
|---|---|---|---|---|---|
| **GMX** | `imap.gmx.com` | `993` / Implicit TLS | `mail.gmx.com` | `587` / STARTTLS (or `465` TLS) | Enable IMAP/POP3 in GMX Webmail Settings |
| **Web.de** | `imap.web.de` | `993` / Implicit TLS | `smtp.web.de` | `587` / STARTTLS | Enable IMAP/POP3 in Web.de Settings |
| **Zoho Mail** | `imap.zoho.com` | `993` / Implicit TLS | `smtp.zoho.com` | `465` / Implicit TLS | Enable IMAP in Zoho Settings; use App Password if 2FA is on |
| **Yandex** | `imap.yandex.com` | `993` / Implicit TLS | `smtp.yandex.com` | `465` / Implicit TLS | Enable IMAP in Yandex Settings; use App Password |
| **Proton Mail Bridge** (LAN) | `<PC-LAN-IP>` | `1143` / STARTTLS | `<PC-LAN-IP>` | `1025` / STARTTLS | Export Bridge's self-signed cert to `ux0:data/VitaMail/ca_cert.pem` (note: Bridge only allows localhost by default unless proxied on LAN) |
| **Self-Hosted (Dovecot/Postfix)** | `mail.yourdomain.tld` | `993` / Implicit TLS | `mail.yourdomain.tld` | `465` TLS or `587` STARTTLS | Let's Encrypt certs work automatically with iTLS-Enso |

### Using a Custom CA Certificate
If you run a private mail server with a self-signed CA, you can place a PEM bundle at `ux0:data/VitaMail/ca_cert.pem` (VitaMail checks `vs0:/data/external/cert/CA_LIST.cer` first, then `vs0:data/external/cert/CA_LIST.cer`, then `ux0:data/VitaMail/CA_LIST.cer`, then `ux0:data/VitaMail/ca_cert.pem`).
