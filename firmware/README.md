# Firmware

The TBS 5580 needs two firmware files, which are **not** redistributed in this
repository because they are proprietary (TBS / Silicon Labs):

- `dvb-usb-id5580.fw` — Cypress FX2 USB controller firmware
- `dvb-demod-si2183-b60-01.fw` — Si2183 demodulator firmware

## How to get them

They ship with the official TBS Linux driver package. Download the TBS
`media_build` / Linux Open Source drivers for the TBS 5580 from TBS
(https://www.tbsdtv.com/, "Downloads" for your card), then extract the tuner
firmware archive it contains:

```sh
# inside the TBS media_build directory
tar xjf tbs-tuner-firmwares_v1.0.tar.bz2 \
    dvb-usb-id5580.fw dvb-demod-si2183-b60-01.fw
cp dvb-usb-id5580.fw dvb-demod-si2183-b60-01.fw /path/to/tbs5580-macos/firmware/
```

If you already use the card on Linux, the same files are usually installed in
`/lib/firmware/`.

Place both files in this `firmware/` directory. The program also accepts
`--fw-dir <dir>` if you keep them elsewhere.
