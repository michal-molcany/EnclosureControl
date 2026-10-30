# Agent rules for this repository

## OctoPrint: back up before you change it

**Always create an OctoPrint backup before installing, updating or uninstalling a
plugin, or before changing OctoPrint settings.** A plugin install can leave the
server unable to start; the backup is the recovery path and must exist *before*
the change, not after.

Live server: `http://10.201.93.88` (port 80). The API key is not in this repo —
ask the user for it. Every call below needs `-H "X-Api-Key: <key>"`.

```powershell
# 1. start a backup (HTTP 201, returns {"name": "...", "started": true})
curl.exe -s -X POST -H "X-Api-Key: $k" -H "Content-Type: application/json" `
  -d "{}" http://10.201.93.88/plugin/backup/backup

# 2. wait: poll until backup_in_progress is false (takes ~20 s)
curl.exe -s -H "X-Api-Key: $k" http://10.201.93.88/plugin/backup/

# 3. download it OUTSIDE the repo and keep it
curl.exe -s -H "X-Api-Key: $k" -o "$env:USERPROFILE\OctoPrintBackups\<name>" `
  "http://10.201.93.88/plugin/backup/download/<name>"
```

Notes:

- `POST /plugin/backup/` is **not** the create route (405). The create route is
  `POST /plugin/backup/backup`.
- Do not commit backups to the repository; `%USERPROFILE%\OctoPrintBackups` is
  the agreed location.
- Plugin install from a local build: `POST /plugin/pluginmanager/upload_file`
  (multipart `file=@<zip>`), then `POST /api/system/commands/core/restart`.
- Verify a plugin load with its own API, not just the plugin list, e.g.
  `POST /api/plugin/enclosure_fan {"command":"status"}`.

## Firmware

- Bump `ENC_FW_VERSION_PATCH` in `lib/EnclosureProto/src/enclosure_version.h` for
  **every** firmware change, then build and OTA-flash the C3.
- Build: `& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-c3-devkitm-1`
  from `EnclosureFanControl`.
- The C3 board currently lives at `10.201.93.245` (the OctoPrint server is
  `10.201.93.88`; they are on different subnets, so mDNS does not cross).
- Keep the READMEs in `EnclosureFanControl/`, `lib/EnclosureUi/`,
  `lib/OctoPrinter/`, `PrinterDisplay/` and `octoprint/` current with every
  behaviour change.
