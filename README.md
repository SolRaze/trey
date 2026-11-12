trey

Old 3DS on Luma3DS driven from a Mac | Relay app on the console | Azahar streams its games over Artic Base

contents
- relay/ console app + Luma plugin, fork of ArticBaseServer | FTP server, SITE commands, CIA install, save sync, updates from this repo's releases
- trey/ Mac CLI | files over FTP | SITE commands | screenshots | CIA install | 3dslink | input over Rosalina InputRedirection | `trey --help`
- flac-scan.py/ FLAC on the card above 16-bit 48 kHz, the Old 3DS limit

relay
rows | Broadcast a game | Saves, Install from inbox | Wi-Fi, Update, Theme | START exits | B back
- broadcast/ arms the Artic Base plugin | connect from Azahar, pick the game on HOME
- sync/ every installed app's save and extdata both ways with `sdmc:/3ds/relay/saves/<TID>.sav` `.ext` | last synced hash per title in `sdmc:/3ds/relay/sync` | one side changed = it wins | both changed = console wins | runs at launch, before Broadcast, Sync now | computer side `trey saves pull|push`
- backups/ replaced copy kept as `saves/old/<TID>/<YYYYMMDD-HHMMSS>.sav` `.ext` | newest 10 per kind | stamp from the console clock
- saves page/ Sync now | titles by name, copy time | A opens backups | A twice restores one | console's copy kept as a backup first | `saves/in/<TID>.sav` `.ext` always wins, same path
- checkpoint/ its backups in `/3ds/Checkpoint/saves|extdata/<0xID name>/<folder>/` listed and restorable | `sdmc:/3ds/relay/checkpoint` present = every relay backup also written there
- limits/ archive over 8 MB skipped, logged | extdata id = unique id, titles with a moved id sync no extdata
- inbox/ installs every CIA in `sdmc:/3ds/relay/inbox`, moves each to `done` | drop files there over FTP or on the card
- wifi/ network, bars, IP on top | saved connections 1 to 3, A connects to one | Wi-Fi off and on | Internet settings opens System Settings
- update/ latest release of this repo, `build` + `relay.cia` | checked at launch | installs only a newer stamp
- https/ libcurl + mbedTLS, roots embedded at build | the console's own TLS fails GitHub's handshake
- build/ devkitPro 3ds-dev + 3ds-curl | `3gxtool` `bannertool` `makerom` on PATH | `make -C relay` | `make -C relay release` uploads
- build id/ `<commit>[+]@<yymmddhhmm>` | `+` dirty tree | stamp orders updates
- SITE/ `trey site help` | SYNC INBOX UPDATE run Sync now, Install, Update remotely
- themes/ `mono` | `memcard` | choice in `sdmc:/3ds/relay/theme`

gotchas
- HOME caches icons per title version | new icon needs a version bump
- AM's CIA file fails unaligned writes | HTTP body written in 64 KB pieces
- Artic Base port 5543 takes one client then stops listening | never probe it
- Rosalina menu open = InputRedirection deaf, FTP down

license mit
