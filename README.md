trey

Old 3DS on Luma3DS driven from a Mac | Relay app on the console | Azahar on a Linux box streams its games

contents
- relay/ console app + Luma plugin, fork of ArticBaseServer | FTP server, SITE commands, CIA install, save sync, updates from this repo's releases
- relay/knave.py/ box side over plain HTTP | save sync with Azahar, CIA inbox, Azahar wake | `relay-knave.service`
- trey/ Mac CLI | files over FTP | SITE commands | screenshots | CIA install | 3dslink | input over Rosalina InputRedirection | `trey --help`
- bottom.py/ Azahar's bottom screen to a tablet while a game runs | touches back as clicks
- flac-scan.py/ FLAC on the card above 16-bit 48 kHz, the Old 3DS limit

relay
rows | Broadcast a game | Sync saves | Install from knave | Update | Theme | Exit
- broadcast/ arms the Artic Base plugin, wakes Azahar on the box, pick the game on HOME
- sync/ every installed app's save both ways with Azahar | last synced hash per title in `sdmc:/3ds/relay/sync` | one side changed = it wins | both changed = console wins | replaced copy kept on the box
- inbox/ installs every CIA in the box's `/srv/relay/inbox`, moves each to `done`
- update/ latest release of this repo, `build` + `relay.cia` | checked at launch | installs only a newer stamp
- https/ libcurl + mbedTLS, roots embedded at build | the console's own TLS fails GitHub's handshake
- build/ devkitPro 3ds-dev + 3ds-curl | `3gxtool` `bannertool` `makerom` on PATH | `make -C relay` | `make -C relay release` uploads
- build id/ `<commit>[+]@<yymmddhhmm>` | `+` dirty tree | stamp orders updates
- SITE/ `trey site help` | SYNC INBOX UPDATE run the rows remotely
- themes/ `mono` | `memcard` | choice in `sdmc:/3ds/relay/theme`

gotchas
- HOME caches icons per title version | new icon needs a version bump
- AM's CIA file fails unaligned writes | HTTP body written in 64 KB pieces
- Artic Base port 5543 takes one client then stops listening | never probe it
- Rosalina menu open = InputRedirection deaf, FTP down

license mit
