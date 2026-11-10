# The header's tournament trophy

The launcher header can show a glowing gold trophy after the tab icons. Clicking it opens a web page in the browser (for a tournament, `https://start.gg/catfight`). The server decides whether it shows and where it goes, so turning it on or off needs no commit or launcher release.

- The accounts service reads `TOURNAMENTS_URL` from `/etc/ppserver/accounts.env` when it starts.
- `GET https://brawlonline.net/v1/launcher` answers `{"tournamentsUrl": "<the URL>"}`, or `{"tournamentsUrl": null}` when the variable is empty or unset. It needs no login.
- Each launcher asks at start and then every 10 minutes. It shows the trophy only for an `http://` or `https://` URL, and with no answer (offline, server down) it keeps its last one (none at start).

Unset is the default, so there is no trophy until you set the variable. Launchers released before this change always show the trophy linking to start.gg/catfight, until they update.

## Turn it on, or change the link

```sh
ssh debian@sarahvps2
sudo -e /etc/ppserver/accounts.env
#   add or edit the line:
#   TOURNAMENTS_URL=https://start.gg/catfight
sudo systemctl restart pp-accounts
curl -s https://brawlonline.net/v1/launcher      # {"tournamentsUrl":"https://start.gg/catfight"}
```

## Turn it off

Leave the value empty (`TOURNAMENTS_URL=`) or delete the line, then restart:

```sh
sudo -e /etc/ppserver/accounts.env
sudo systemctl restart pp-accounts
curl -s https://brawlonline.net/v1/launcher      # {"tournamentsUrl":null}
```

## Notes

- Open launchers update within 10 minutes; a launcher that starts gets the new answer right away.
- Restarting `pp-accounts` is what every server deploy does already. Launchers reconnect by themselves.
- `sudo -e` keeps the file's owner and mode (`root:ppserver 0640`). Don't print the whole file anywhere public: it holds the secrets.
- A value that is not an http(s) URL (a typo like `start.gg/catfight` without `https://`) counts as empty: no trophy. Check the `curl` output after a change.
- Locally or in staging, set `TOURNAMENTS_URL` in the accounts server's environment (`server/.env`, or the shell that starts it).
- Code: `server/crates/accounts/src/launcher.rs` (the endpoint), `launcher/src/renderer/lib/hooks/use_tournaments_url.ts` (the launcher's polling), `TournamentsButton` in `launcher/src/renderer/app/header/header.tsx`.
