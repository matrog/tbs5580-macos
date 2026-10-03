# tbs5580-macos

A user-space DVB-S/S2 driver for the **TurboSight TBS 5580** USB tuner on
**macOS**. It talks to the card entirely through libusb — no kernel extension,
no DriverKit, no signing — and gives you channel scanning, a full-screen
terminal UI, and transport-stream output to VLC, a file, UDP or HTTP.

It is a user-space port of the Linux TBS `media_build` drivers (the Cypress FX2
bridge, the Silicon Labs Si2183 demodulator and the Airoha AV2018 tuner), plus
the Si2183 hardware blind-scan code from neumoDVB.

> Satellite only. DVB-T/T2/C (the card's terrestrial/cable tuner) and the CI/CAM
> slot are **not** implemented. See [Limitations](#limitations).

## Features

- **DVB-S / DVB-S2** tuning, including multistream (ISI / PLS gold & root).
- **LNB control**: 13/18 V, 22 kHz tone, universal or custom LOFs, C band.
- **DiSEqC 1.0** committed switch, and **DiSEqC 1.2 / USALS** motor support.
- **Channel scanning**:
  - single transponder,
  - whole satellite following the NIT (network information table),
  - **hardware blind scan** (the Si2183 finds the transponders by itself).
- **Full-screen TUI** (`-i`): search, satellite filter, sorting, live signal
  (lock, C/N, dBm, bitrate), and one-key play to VLC.
- **Transport-stream output**: to a file, to stdout, over UDP, or over a small
  built-in **HTTP server** (whole mux, a single service, or an M3U playlist).
- A per-channel list stored as a simple tab-separated `channels.conf`, with the
  satellite and rotor position of each channel.

## Requirements

- macOS (tested on Intel; should work on Apple Silicon).
- [libusb](https://libusb.info/) and `pkg-config` (via Homebrew).
- A C compiler (the Xcode command-line tools).
- `ncurses` and `iconv` ship with macOS — nothing to install.

```sh
brew install libusb pkg-config
```

## Build

```sh
make
```

This produces the `tbs5580` binary. `make install` copies it to
`/usr/local/bin` and the firmware to `/usr/local/share/tbs5580`.

### Firmware

The card needs two firmware files, included in `firmware/`:

- `dvb-usb-id5580.fw` — the Cypress FX2 USB controller firmware.
- `dvb-demod-si2183-b60-01.fw` — the Si2183 demodulator firmware.

They are loaded automatically. The program looks for them in `./firmware`, next
to the binary, in `<binary>/../share/tbs5580`, or in a directory you pass with
`--fw-dir`.

### Hardware notes

- The TBS 5580 needs its **external 12 V power supply** — USB power alone is not
  enough once the LNB draws current, and the card may drop off the USB bus
  otherwise.
- Prefer a **direct USB connection** (a plain USB‑C → USB‑A adapter is fine)
  rather than going through a dock/hub.

## Usage

```sh
# check the hardware: firmware, EEPROM/MAC, chip revision
./tbs5580 --info -v

# scan a satellite starting from one known transponder, following the NIT
./tbs5580 --scan -f 11766 -p H -s 27500

# hardware blind scan of the whole satellite, then scan the channels
./tbs5580 --blindscan

# interactive full-screen chooser (plays in VLC)
./tbs5580 -i
```

### Interactive TUI (`-i`)

A full-screen, ncurses-based chooser:

| Key          | Action                                                   |
|--------------|----------------------------------------------------------|
| ↑ / ↓        | move; PgUp/PgDn page; Home/End jump                      |
| *type text*  | live search by channel name or provider                  |
| Enter        | tune and play the selected channel in VLC                |
| `Ctrl-F`     | filter by satellite (pick one, or All)                   |
| `Ctrl-O`     | change sort order: name → frequency → satellite          |
| `Ctrl-N` / F2| open the **scan menu** (see below)                       |
| `Ctrl-A`     | show/hide scrambled and data services                    |
| Esc          | clear the search, or quit if the search is empty         |

Scrambled channels are shown with a `$` in front of the name. The bottom line
shows the live signal of the channel being played. Log messages (USB, HTTP,
tuning) go to `tbs5580.log` so they don't clutter the screen.

#### Scan menu (`Ctrl-N`)

The scan menu targets the satellite of the selected channel (with its rotor
position), or a new satellite you enter. It offers:

1. **Single transponder** — enter frequency, polarization, symbol rate.
2. **Whole satellite from known TPs (NIT)** — start from one TP and follow the
   network table.
3. **Blind scan** — discover every transponder of the satellite.
4. **Change target satellite…** — scan a satellite not yet in the list, giving
   its DiSEqC position (`N`), USALS (`usals:LAT,LON`) or none (`-`).

Progress is shown on screen; `Esc`/`q` stops. A scan updates only the
transponders it actually sees and never wipes the rest of the list.

### Direct tuning and output

```sh
# measure the signal only (dish alignment), no output
./tbs5580 -f 11766 -p H -s 27500

# serve the transport stream over HTTP:
#   /               the whole transponder
#   /<sid>          a single service
#   /playlist.m3u   the channels of the transponder (open it in VLC)
./tbs5580 -f 11766 -p H -s 27500 -H 8001

# a single service to stdout into VLC, or the whole mux to a file
./tbs5580 -f 11766 -p H -s 27500 -S 3401 -o - | /Applications/VLC.app/Contents/MacOS/VLC -
./tbs5580 -f 12188 -p H -s 27500 -D 2 -o mux.ts -t 30
```

Useful options: `-d S|S2|auto`, `--lnb universal|LOF|LOF1,LOF2,SLOF`,
`-D 1-4` (DiSEqC 1.0), `--isi`/`--pls-gold`/`--pls-root` (multistream),
`-u host:port` (UDP), `-c FILE` (channel list), `-t SEC` (stop after N seconds).
Run `./tbs5580 -h` for the full list.

### Motorized dish (DiSEqC 1.2 / USALS)

```sh
# USALS: site coordinates + target satellite
./tbs5580 --usals 45.46N,9.19E --sat 19.2E --blindscan

# DiSEqC 1.2: stored positions
./tbs5580 --positions 13E=1,19.2E=2 --sat 13E -f 11013 -p H -s 29900

# manual positioning commands (dish alignment)
./tbs5580 --motor west:5 -f 11013 -p H -s 29900
./tbs5580 --motor halt
```

`--motor` commands: `halt`, `east[:N]`/`west[:N]` (N steps), `drive-east:SEC`,
`drive-west:SEC`, `goto:N`, `store:N`, `reference`, `gotox:POS` (USALS),
`limit-east`, `limit-west`, `limits-off`.

Each channel in `channels.conf` remembers how to point the dish (a DiSEqC 1.2
stored position, or USALS), so in the TUI, choosing a channel on another
satellite moves the dish automatically. You can set it for an already-scanned
satellite without rescanning:

```sh
./tbs5580 --set-position 13E=1
./tbs5580 --set-position 13E=usals:45.46N,9.19E
```

Default options can be put in a `tbs5580.rc` file in the working directory, one
option per line (for example `--usals 45.46N,9.19E`).

## Files written at runtime (working directory)

- `channels.conf` — the channel list (tab-separated text).
- `tbs5580_rotor.state` — the last position the dish was sent to.
- `tbs5580.log` — log output while the TUI is running.

## Limitations

- **Satellite only.** The card's terrestrial/cable tuner (Si2157, DVB-T/T2/C)
  is not implemented.
- **No CI/CAM.** The transport stream is routed around the CI slot; pay-TV
  decryption with a CAM is not supported.
- **No descrambling.** Encrypted channels are listed (with a `$`) but are not
  decrypted.

## License

GPL-2.0. This is a port of GPL-2 code:

- the TBS Linux drivers (`media_build`: `tbs5580.c`, `si2183.c`, `av201x.c`),
- the Si2183 hardware blind-scan code from
  [deeptho/linux_media](https://github.com/deeptho/linux_media) (neumoDVB).

See the source headers for the original copyright notices.

## Disclaimer

This is an independent project and is not affiliated with or endorsed by
TurboSight. Use it in accordance with the laws and regulations that apply to
you, and only receive signals you are authorized to receive.
