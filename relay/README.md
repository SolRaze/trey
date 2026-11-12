relay

trey's console app + Luma plugin | fork of ArticBaseServer v1.2.0 | github.com/azahar-emu/ArticBaseServer

parts
- app/ Relay launcher, citro2d menu | FTP on 5000 | rows broadcast, sync, inbox, update, theme, exit
- plugin/ Luma 3GX in the game process | Artic Base server 5543 + FTP 5000
- common/ FTP server, SITE commands, CIA install sink | `common/test/host.sh` tests it on the Mac

build
devkitPro 3ds-dev + 3ds-curl | `3gxtool` `bannertool` `makerom` on PATH | `make` | `make release` to GitHub
CA roots from `CA`, default `/etc/ssl/cert.pem`

license mit | upstream terms in LICENSE | changes in NOTICE
