# JUNGLE

JUNGLE turns the M-VAVE FM-1 into a breakbeat slicer. Load a break from the web page, and the FM-1 cuts it into
sixteenths and keeps rearranging it in time as it plays: shuffled beats, stutters, reversed hits, dropouts and fills
every few bars, as much or as little as CHAOS says. It follows a MIDI clock, and the keys play it live.

![JUNGLE playing a two-bar break: the waveform with its slices, the pattern as it is now, the HOME knobs](manual/img/screen-playing.png "JUNGLE on the FM-1's screen")

Version {{VERSION}}. Installing is at your own risk; the installer can put M-VAVE's own firmware back.

- [Install](install/): from Chrome or Edge, with the FM-1 connected by USB.
- [Load a break](loader/): drop a loop in, check its tempo and slices, send it to the FM-1. It stays there through
  power-off, until you send another.
- [Manual](manual/): the screen, the controls, MIDI.
- [Try it in the browser](emu/): the firmware itself, without a break (the loader needs a real FM-1).
- [Download {{PKG}}](firmware/{{PKG}}): for the command-line installer.
- [Source](https://github.com/jose-no/Fm1-jungle): GPL-3.0. Built on fm1-base (Charles Vestal) and felucca-bsp (Leo
  Kuroshita, Hügelton Instruments).

M-VAVE and FM-1 are trademarks of their owners. JUNGLE is not affiliated with M-VAVE.
