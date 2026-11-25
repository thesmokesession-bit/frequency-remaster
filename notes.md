Version 657743534, built from commit 2affc631. Security update. Install this build, because the
signature check in the previous build refused genuine releases on the console.

## Changes

- Signed updates. The in-game updater installs a release only when its `eboot.bin.sig` is a valid
  signature from the release key. The key is not stored on GitHub, and a stolen GitHub login
  cannot put code on the console.
- OpenSSL 3.5.9 in place of 3.5.2 for every HTTPS connection the game makes.
- Room and scoreboard codes use 256 words instead of 64. Codes from earlier builds still work. L1
  and R1 jump a word to the previous or next letter.

## Installing

For a new install, unzip `Frequency-PS5-657743534.zip`, put your ISO of _FreQuency_ (USA) in the
`PPSA64010` folder, and copy the folder to `/data/homebrew/` on the PS5.

To update, copy `eboot.bin` over `/data/homebrew/PPSA64010/eboot.bin`, or let the in-game updater
fetch the build on its next start.

No game data is included. The
[README](https://github.com/thesmokesession-bit/frequency-remaster#installing-on-a-ps5) has the
full steps and the command to verify `eboot.bin.sig`.
