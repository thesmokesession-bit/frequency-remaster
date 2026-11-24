#!/usr/bin/env python3
"""Unpack a song from the Frequency disc image into a song folder, and pack song folders into arks.

The host serves every song folder under FREQ_SONGS as if its files were in the disc's arks (see
host/io/disc.cpp). ``pack`` writes the same merged arks as files, for a rebuilt disc image.

    python3 -I host/io/songs.py list <image>
    python3 -I host/io/songs.py unpack <image> <song> <songs-dir> [--as <name> --id <n>]
    python3 -I host/io/songs.py pack <image> <songs-dir> <out-dir>
    python3 -I host/io/songs.py test <image> <scratch-dir>

The disc
--------

The image is ISO 9660. The game's data is in four arks under \\ARK (ROOT, LEVELS, ARENAS, and
LOADING). An ark starts with a 256-byte header of little-endian words: "ARK\\0", version 2, then
the offset and count of the file table, of the directory table, and of the string table, the end
of the three tables, the sector size (2048), and an optimized-order block (unused on this disc).
The header path from byte 0x40 ("C:/FREQ/run/Levels") gives the mount point, the lowercased part
after "run": "", "levels", "arenas", and "loading". A game path such as
levels/dubpistols/dubpistolsf1_c.mid goes to the ark with the longest matching mount point, and
the rest splits into a directory ("dubpistols") and a name. The header and the tables must end
inside the first 64 KiB of the ark, the part the game copies them from.

A file entry is 24 bytes: the name's hash, flags (0), the name's offset in the ark, the
directory's index, the byte offset inside the first sector, the sector, the stored length, and
the inflated size (the gzip trailer's for a .gz file, else the length). A directory entry is 8
bytes: the path's hash, flags (0), and the path's offset. The hash is HashArkString(). It shifts
each character one more place than the last (wrapping after 7) and combines them with exclusive
or.

A song
------

A song is data the game's Python 2.0 interpreter reads, not code in the program:

- levels/album.py lists the songs in levels_database as (name, stage, order). Stages 1 to 5 are
  the solo stages, 6 has the secret songs (shown after stage 5's), and order sorts a stage's
  list. A stage screen row has seven song indicators (five songs and the two secret ones).
  Stages 1 to 4 therefore have room for two more songs each (scroll right past the fifth), stage
  5's row is full, and an eighth song in a row runs past the indicators. The sixth and seventh
  places stay locked until the two secret songs unlock (FREQ_UNLOCK_ALL=1 opens them at once).
- levels/<name>/<name>.py defines the Level subclass: directory_name (the folder), the song's
  sections (section_boundaries in bars), which tracks enable which, the catch rules, the
  instrument materials of the eight tracks, the text the menus show (get_name, get_artist, and
  others), get_song_id (unique; saves and high scores are looked up by it), and the SPU2 effect.
- levels/<name>/<name>f1_c.mid is the game chart and f1_p.mid the remix and jam one: standard
  MIDI files, format 1, 480 ticks a quarter note. Tracks "T1 ..." to "T8 ..." have each tunnel
  track's gems (notes 60 to 91, one octave per difficulty). "Tn CATCH", "Tn DATA", "BG_...", and
  "INTRO" tracks play the music through the sampler. The song's audio is MIDI.
- levels/<name>/<name>.bd and .hd are the sample bank: SPU2 ADPCM samples, and a Sony "IECS"
  version 2 header that maps programs and notes onto them. A song with bank swapping has one
  pair per section instance (<name>_1.bd ...) and a <name>.mmv bank movie. The largest bank on
  the disc is 1829376 bytes (xlr8r.bd).
- levels/<name>/images/gen/ has the pictures. <name>_pic_bmp.abm.gz and <name>_logo_bmp.abm.gz
  are for the menus (named after the song; a missing picture falls back to a shared one).
  images.rnd.gz is the song's scene in the tunnel and refers to its movies (*_gif.mmv) by file
  name.

A song has no tunnel files. The tunnel is an arena under arenas/, chosen separately.

A song folder
-------------

A song folder has files at their game paths, for example
<songs-dir>/dubpistols/levels/dubpistols/dubpistolsf1_c.mid. A file replaces the disc's file of
the same path or adds a new one. A file <path>.append is added to the end of <path> instead,
after every folder's replacement. Several new songs can each extend levels/album.py this way.
Folders are taken in name order, and a later one wins.

A new song
----------

A new song's folder needs levels/<name>/<name>.py, <name>f1_c.mid, <name>f1_p.mid, <name>.bd,
<name>.hd, and a levels/album.py.append line that adds (name, stage, order) to levels_database.
``unpack --as`` writes these from a disc song (stage 2, sixth place). An author of a song made
from scratch provides these files:

- the samples as a bank in the .bd/.hd format above. This tool does not write banks, and an
  external SPU2 bank builder is needed;
- a chart with a disc chart's layout: the conductor track first (tempo and time signature), then
  for each tunnel track its gem track, its CATCH track (notes that play the bank's programs; T7
  has SCRATCH, and T8 has AXE and HARMONY), and its DATA track, then the BG_ and INTRO tracks.
  This tool does not check a chart, and a copy of a disc chart with its notes replaced is the
  safe start;
- the level script, from a disc song's, with section_boundaries matching the chart's bars, a new
  song id, the menu text, and track materials;
- the images/gen files. A missing menu picture falls back to a shared one, and a song without
  images.rnd.gz is untested. A disc song's files (as ``unpack --as`` copies them) work, showing
  the disc song's pictures.
"""
import os
import re
import shutil
import struct
import sys

SECTOR = 2048
CHUNK = 0x10000
HEADER = 256
ARKS = ('ROOT', 'LEVELS', 'ARENAS', 'LOADING')


def iso_find(image, path):
    """The (offset, size) of a file such as ARK/LEVELS.ARK in an ISO 9660 image."""
    image.seek(16 * SECTOR)
    root = image.read(SECTOR)[156:190]
    lsn, size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
    for component in path.upper().split('/'):
        image.seek(lsn * SECTOR)
        data = image.read(size)
        found = None
        offset = 0
        while offset < len(data) and found is None:
            length = data[offset]
            if length == 0:
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            name = data[offset + 33:offset + 33 + data[offset + 32]].split(b';')[0]
            if name.decode('latin-1').upper() == component:
                found = (struct.unpack_from('<I', data, offset + 2)[0],
                         struct.unpack_from('<I', data, offset + 10)[0])
            offset += length
        if found is None:
            sys.exit(f'{path} is not in the image')
        lsn, size = found
    return lsn * SECTOR, size


def ark_hash(text):
    value = 0
    for i, character in enumerate(text.encode('latin-1')):
        value ^= character << (i & 7)
    return value & 0xffff


def inflated_size(name, data):
    return struct.unpack('<I', data[-4:])[0] if name.endswith('.gz') else len(data)


class Ark:
    """An ark at an offset of a file: its mount point and its files, (directory, name) -> entry."""

    def __init__(self, image, offset, size):
        self.image = image
        self.offset = offset
        self.size = size
        image.seek(offset)
        self.header = image.read(CHUNK)
        (signature, version, files_at, file_count, dirs_at, dir_count, _, _, self.end, sector_size,
         optimized) = struct.unpack_from('<4s10i', self.header)
        assert signature == b'ARK\0' and version == 2 and sector_size == SECTOR and not optimized
        path = self.string(0x40).lower()
        self.mount = path.split('/run', 1)[1].lstrip('/')
        self.dirs = [self.string(struct.unpack_from('<HHi', self.header, dirs_at + i * 8)[2])
                     for i in range(dir_count)]
        self.hashes = {}
        self.files = {}
        for i in range(file_count):
            name_hash, _, at, directory, in_sector, sector, length, size = struct.unpack_from(
                '<HHihHiii', self.header, files_at + i * 24)
            key = self.dirs[directory], self.string(at)
            self.files[key] = (sector * SECTOR + in_sector, length, size)
            self.hashes[key] = name_hash

    def string(self, at):
        return self.header[at:self.header.index(b'\0', at)].decode('latin-1')

    def game_path(self, key):
        return '/'.join(part for part in (self.mount, *key) if part)

    def read(self, key):
        at, length, _ = self.files[key]
        self.image.seek(self.offset + at)
        return self.image.read(length)


def open_arks(image_path):
    image = open(image_path, 'rb')
    return [Ark(image, *iso_find(image, f'ARK/{name}.ARK')) for name in ARKS]


def ark_key(arks, game_path):
    """The ark and the (directory, name) a lowercased game path maps to."""
    directory, _, name = game_path.rpartition('/')
    best = max((ark for ark in arks if ark.mount == '' or directory == ark.mount or
                directory.startswith(ark.mount + '/')),
               key=lambda ark: len(ark.mount))
    return best, (directory[len(best.mount):].lstrip('/'), name)


def merged_files(arks, songs_dir):
    """For each ark, its added or changed files as {(directory, name): bytes}."""
    replace = {}
    append = {}
    for folder in sorted(os.listdir(songs_dir)):
        top = os.path.join(songs_dir, folder)
        for directory, subdirectories, names in os.walk(top):
            subdirectories.sort()
            for name in sorted(names):
                path = os.path.join(directory, name)
                game_path = os.path.relpath(path, top).replace(os.sep, '/').lower()
                if game_path.endswith('.append'):
                    append.setdefault(game_path[:-len('.append')], []).append(path)
                else:
                    replace[game_path] = path
    changed = {id(ark): {} for ark in arks}
    for game_path in sorted(set(replace) | set(append)):
        ark, key = ark_key(arks, game_path)
        if game_path in replace:
            data = open(replace[game_path], 'rb').read()
        else:
            data = ark.read(key) if key in ark.files else b''
        for path in append.get(game_path, ()):
            data += open(path, 'rb').read()
        changed[id(ark)][key] = data
    return changed


def write_ark(ark, changed, out):
    """Write ark with changed files: new tables, the old ark 64 KiB on, then the files."""
    entries = {key: (at + CHUNK, length, size) for key, (at, length, size) in ark.files.items()}
    data_at = CHUNK + (ark.size + SECTOR - 1) // SECTOR * SECTOR
    for key, data in changed.items():
        entries[key] = (data_at, len(data), inflated_size(key[1], data))
        data_at += (len(data) + SECTOR - 1) // SECTOR * SECTOR
    dirs = ark.dirs + sorted({key[0] for key in entries} - set(ark.dirs))
    files_at = HEADER
    dirs_at = files_at + 24 * len(entries)
    strings_at = dirs_at + 8 * len(dirs)
    strings = bytearray()
    string_at = {}
    for text in [key[1] for key in entries] + dirs:
        if text not in string_at:
            string_at[text] = strings_at + len(strings)
            strings += text.encode('latin-1') + b'\0'
    end = strings_at + len(strings)
    if end > CHUNK:
        sys.exit(f'{ark.mount or "root"} ark: the tables need {end} bytes, more than {CHUNK}')
    tables = bytearray(CHUNK)
    tables[:HEADER] = ark.header[:HEADER]
    struct.pack_into('<8i', tables, 8, files_at, len(entries), dirs_at, len(dirs), strings_at,
                     len(entries) + len(dirs), end, SECTOR)
    dir_index = {text: i for i, text in enumerate(dirs)}
    for i, (key, (at, length, size)) in enumerate(entries.items()):
        struct.pack_into('<HHihHiii', tables, files_at + i * 24, ark_hash(key[1]), 0,
                         string_at[key[1]], dir_index[key[0]], at % SECTOR, at // SECTOR, length,
                         size)
    for i, text in enumerate(dirs):
        struct.pack_into('<HHi', tables, dirs_at + i * 8, ark_hash(text), 0, string_at[text])
    tables[strings_at:end] = strings
    out.write(tables)
    ark.image.seek(ark.offset)
    for left in range(ark.size, 0, -(1 << 20)):
        out.write(ark.image.read(min(left, 1 << 20)))
    out.write(b'\0' * (-ark.size % SECTOR))
    for data in changed.values():
        out.write(data + b'\0' * (-len(data) % SECTOR))
    # The ark ends on a whole 64 KiB chunk. The game's asynchronous loader reads whole chunks.
    out.write(b'\0' * (-data_at % CHUNK))


def command_list(image_path):
    for ark in open_arks(image_path):
        for key in ark.files:
            print(ark.game_path(key))


def command_unpack(image_path, song, songs_dir, new_name=None, song_id=None):
    levels = open_arks(image_path)[ARKS.index('LEVELS')]
    keys = [key for key in levels.files if key[0] == song or key[0].startswith(song + '/')]
    if not keys:
        sys.exit(f'no song {song} in the levels ark')
    name = new_name or song
    top = os.path.join(songs_dir, name)
    for key in keys:
        data = levels.read(key)
        directory, base = name + key[0][len(song):], key[1]
        # The game builds these names from the song's; images.rnd names the other pictures.
        if (directory == name or base.endswith('_bmp.abm.gz')) and base.startswith(song):
            base = name + base[len(song):]
        path = levels.game_path((directory, base))
        if new_name and key == (song, f'{song}.py'):
            text, count = re.subn(r'(def get_song_id \(self\):\s*return )\d+', rf'\g<1>{song_id}',
                                  data.decode('latin-1').replace(song, name))
            assert count == 1, 'the script has no get_song_id to renumber'
            data = text.encode('latin-1')
        os.makedirs(os.path.dirname(os.path.join(top, path)), exist_ok=True)
        open(os.path.join(top, path), 'wb').write(data)
    if new_name:
        open(os.path.join(top, 'levels', 'album.py.append'), 'w').write(
            f'\nlevels_database = levels_database + ((\'{name}\', 2, 6),)\n')
    print(f'{len(keys)} files of {song} in {top}')


def command_pack(image_path, songs_dir, out_dir):
    arks = open_arks(image_path)
    changed = merged_files(arks, songs_dir)
    os.makedirs(out_dir, exist_ok=True)
    for ark, name in zip(arks, ARKS):
        if changed[id(ark)]:
            with open(os.path.join(out_dir, f'{name}.ARK'), 'wb') as out:
                write_ark(ark, changed[id(ark)], out)
            print(f'{name}.ARK: {len(changed[id(ark)])} files from {songs_dir}')


def command_test(image_path, scratch):
    shutil.rmtree(scratch, ignore_errors=True)
    songs = os.path.join(scratch, 'songs')
    command_unpack(image_path, 'dubpistols', songs)
    command_unpack(image_path, 'dubpistols', songs, 'dubcopy', 99)
    open(os.path.join(songs, 'dubpistols', 'levels', 'dubpistols', 'dubpistolsf1_c.mid'),
         'ab').write(b'changed')
    arks = open_arks(image_path)
    levels = arks[ARKS.index('LEVELS')]
    assert ark_key(arks, 'levels/album.py') == (levels, ('', 'album.py'))
    assert ark_key(arks, 'global/defaults.py')[0].mount == ''
    assert ark_key(arks, 'arenas/blue/blue.py')[0].mount == 'arenas'
    for ark in arks:
        for key, (_, length, size) in ark.files.items():
            assert ark.hashes[key] == ark_hash(key[1]), key
            if key[1].endswith('.gz'):
                assert inflated_size(key[1], ark.read(key)) == size, key
            else:
                assert length == size, key
    changed = merged_files(arks, songs)
    assert all(not files for ark, files in ((a, changed[id(a)]) for a in arks) if ark != levels)
    changed = changed[id(levels)]
    assert changed[('dubpistols', 'dubpistolsf1_c.mid')].endswith(b'changed')
    assert changed[('dubcopy', 'dubcopy.py')].count(b"'dubcopy'") == 1
    album = changed[('', 'album.py')]
    assert album.startswith(levels.read(('', 'album.py'))) and b"('dubcopy', 2, 6)" in album
    os.makedirs(os.path.join(scratch, 'out'))
    command_pack(image_path, songs, os.path.join(scratch, 'out'))
    path = os.path.join(scratch, 'out', 'LEVELS.ARK')
    packed = Ark(open(path, 'rb'), 0, os.path.getsize(path))
    assert packed.mount == 'levels' and packed.end <= CHUNK and packed.size % CHUNK == 0
    assert set(packed.files) == set(levels.files) | set(changed)
    for key in packed.files:
        assert packed.hashes[key] == ark_hash(key[1]), key
        expected = changed[key] if key in changed else levels.read(key)
        assert packed.read(key) == expected, key
        assert packed.files[key][2] == inflated_size(key[1], expected), key
    print(f'{len(packed.files)} entries read back, {len(changed)} of them from the folders')
    print('SONGS_SELFTEST_OK')


def main():
    commands = {'list': command_list, 'unpack': command_unpack, 'pack': command_pack,
                'test': command_test}
    arguments = sys.argv[1:]
    if not arguments or arguments[0] not in commands:
        sys.exit(__doc__)
    if arguments[0] == 'unpack' and '--as' in arguments:
        at = arguments.index('--as')
        options = dict(zip(arguments[at::2], arguments[at + 1::2]))
        arguments = arguments[:at] + [options['--as'], int(options['--id'])]
    commands[arguments[0]](*arguments[1:])


if __name__ == '__main__':
    main()
