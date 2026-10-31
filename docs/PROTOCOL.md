# The break upload protocol

USB-MIDI SysEx between the loader page and JUNGLE (app/brk.c, web/loader/index.html). Every frame:

    F0 7D 4A 42 CMD payload... F7

`7D` is the non-commercial ID, `4A 42` is "JB". The payload is 8-bit data packed 7 bytes to 8: a byte holding the
top bits of the next (up to) seven, then those seven with their top bit cleared. Numbers are little-endian. The FM-1
answers every frame with `CMD | 0x40`, the first payload byte a status:

| Status | |
| --- | --- |
| 0 | ok |
| 1 | bad frame |
| 2 | too big for the flash |
| 3 | bad header |
| 4 | flash error |
| 5 | checksum mismatch |
| 6 | out of order |

| CMD | Host sends | FM-1 answers |
| --- | --- | --- |
| 01 HELLO | nothing | version (1), capacity in bytes (u32), a break there (u8), then its len, rate, bpm10, bars, crc (u32 each) and name (24 bytes) |
| 02 BEGIN | the header (320 bytes, below) | status, after erasing the old break and the room for the new one (seconds) |
| 03 DATA | offset in bytes (u32), then up to 448 bytes of samples | status, offset (u32) |
| 04 END | nothing | status, the CRC it computed (u32). The header is written now, only if the samples' CRC matches |
| 05 HEADER | a header for the same samples (len and crc equal to the break's) | status: new slices without sending the audio again |

One frame at a time: the host waits for each answer.

The header (`brk_hdr_t`), 320 bytes: magic `4A 42 52 4B` ("JBRK"), version 1, len (samples), rate (Hz, 8000..48000),
bpm10 (tenths of a BPM, 600..2400), bars (1, 2, 4), nsl (bars × 16), snare (a slice), crc (CRC-32 of the samples'
bytes), name (24 bytes, upper case, 0-terminated), start (64 × u32: each slice's first sample, ascending), hcrc
(CRC-32 of the 316 bytes before it, filled in by the FM-1).

In the flash (fm1-base's data area): the header at 0x97000, the samples (mono, 16-bit) from 0x98000 to 0xDC000.
