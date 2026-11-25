Built from commit 657743534. Security update. Install this one: the signature check in
a73926494 refused genuine releases on the console.

- Signed updates: the in-game updater installs a release only when its eboot.bin.sig is a valid
  signature from the release key, which is kept off GitHub. A stolen GitHub login can no longer
  put code on the console.
- OpenSSL 3.5.9 in place of 3.5.2 for every HTTPS connection the game makes.
- Room and scoreboard codes use 256 words instead of 64. Codes made by earlier builds still work.
  L1 and R1 jump a word to the next or previous letter.

Install: copy eboot.bin over /data/homebrew/PPSA64010/eboot.bin, or let the in-game updater fetch
it on its next start. No game data is included; you need your own copy of Frequency (USA).
