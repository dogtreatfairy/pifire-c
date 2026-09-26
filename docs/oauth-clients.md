# The project's cloud clients

A grill backs up to Google Drive or OneDrive by signing in from the phone: the Backup page shows
a code, the phone enters it at google.com/device or microsoft.com/devicelogin, and the grill
keeps a refresh token scoped to the files it made. For that to be one tap, PiFire ships its own
OAuth clients in `share/oauth-clients.json`, which is embedded in the daemon. Nobody using a
grill should have to open a developer console.

An installed-app client is not a secret in Google's sense ("the client secret is obviously not
treated as a secret" — Google's OAuth docs for installed apps); it identifies the application,
not the user. The user's tokens never leave their grill.

These are made once by the project maintainer and committed. With the fields blank, the Backup
page asks each user for a client of their own instead, which is the fallback and nothing more.

## Google Drive (10 minutes)

1. console.cloud.google.com → create a project, e.g. "PiFire".
2. APIs & Services → Library → enable **Google Drive API**.
3. APIs & Services → OAuth consent screen → **External**. App name "PiFire", your email as the
   support and developer contact. Scopes: add `.../auth/drive.file` only (it is a non-sensitive
   scope: PiFire sees only files it created, and no verification review is needed). Save.
4. **Publish the app** (Publishing status: In production). This matters: an app left in
   "Testing" hands out refresh tokens that expire after seven days, so every grill would
   disconnect weekly.
5. APIs & Services → Credentials → Create credentials → OAuth client ID → application type
   **TVs and Limited Input devices**. Name "PiFire grill".
6. Copy the **Client ID** and **Client secret** into `share/oauth-clients.json` under `gdrive`.

## OneDrive (10 minutes)

1. portal.azure.com → Microsoft Entra ID → App registrations → New registration.
2. Name "PiFire". Supported account types: **Personal Microsoft accounts only** (or "any
   organizational directory and personal accounts" if work accounts should work too). No
   redirect URI. Register.
3. Authentication → Advanced settings → **Allow public client flows: Yes**. Save. (This is what
   makes device sign-in work without a secret.)
4. API permissions → Add → Microsoft Graph → Delegated → `Files.ReadWrite.AppFolder` and
   `offline_access`. No admin consent is needed for these.
5. Overview → copy the **Application (client) ID** into `share/oauth-clients.json` under
   `onedrive`. There is no secret.

## What the daemon does with them

`src/features/backup.c` reads the file through `pf_embedded_share("oauth-clients.json")`. A
location whose own `client_id` is blank uses the project's; one with its own (set under
Advanced on the location's screen) uses that. `GET /backup` reports which built-in clients exist
in `clients`, and the page shows the client fields only when there is none.
