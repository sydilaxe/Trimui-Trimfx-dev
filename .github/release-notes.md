TrimFX, a four-module FX chain (TONE → MOD → DELAY → REVERB) for a USB FX send on the TrimUI Brick Pro,
Brick and Brick Hammer running MustardOS. The Brick Pro's two sticks play the effects as X/Y pads.

**Install on MustardOS**

Copy `TrimFX-mustardos.muxapp` into the `ARCHIVE` folder of the first SD card, install it from
**Applications → Archive Manager**, then open **Applications → TrimFX**. Installing over an older
version keeps your preset slots, `state.txt`, `midimap.txt`, `audio.txt` and the log. Installed copies
also update themselves from here (**SETUP → UPDATES**).

**USB audio: low latency, resilient (brickio)**
- A USB interface is opened direct (`plughw`), skipping the shared dmix/dsnoop path and its ~90 ms floor.
  The round trip is about 25 ms at the default 256-frame buffer, lower at 128 or 64 where the interface keeps up.
- 24-bit / 48 kHz kept end to end; multichannel interfaces (a MixPre) opened whole, any channel pair chosen.
- Input and output can be different interfaces; their clocks' drift is followed by resampling.
- Real-time audio threads, a watchdog, the buffer grown after repeated dropouts, a BUFFER per interface.
- The speaker stays silent with no USB output, so an FX send never plays out of the Brick.
- USB MIDI alongside, over a hub too: a Korg nanoKONTROL Studio template, and MIDI LEARN.

**The effects**
- Several algorithms per module, each with its own DEEP page (F1 / F2 cycle the pages), among them the
  TEMPLE and NEBULA reverbs and the PRISM and ERODE delays.
- Algorithm changes clear their memory off the audio thread, so changing one doesn't click.
- Level meters for input and output, the sticks' rings and the top bar showing the effects.

**Logs**
- `log.txt` records what the interface runs (format, rate, period, DIRECT, real-time), dropouts, clips
  and the load. Set **SETUP → LOG** to DETAIL before testing, and send `log.txt` and `audio.txt`.

The SHA-256 of the package is attached beside it.
