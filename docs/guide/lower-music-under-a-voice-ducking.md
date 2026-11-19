# Lower music under a voice (ducking)

When someone speaks, the music should get quieter by itself, and come back when they stop.
This is called **ducking**. In Kinoklip the music track's compressor listens to the voice
track instead of to the music: whenever the voice is loud, the music is turned down.

You need a voice track and a music track on the timeline, for example the voice on **A1**
and the music on **A2**.

## Set it up

1. Open the **Audio** page (**Shift+7**).
2. On the strip of the **music** track in the mixer, click **Dyn** to open the Dynamics
   window.
3. Tick **Compressor**.
4. Open the **Key** list and choose the voice track (for example **Track A1**). The list
   greys out sources that would make a loop, such as the music track itself or a bus that
   the music feeds.
5. Set the compressor:
   - **Threshold** about **-40 dB**: the music is turned down whenever the voice is louder
     than this.
   - **Ratio** about **6:1** to **10:1**: the higher it is, the more the music drops.
   - **Attack** about **10 ms**, **Release** about **300 ms**: the music goes down quickly
     when the voice starts and comes back slowly, so it does not pump.
6. Press **Play** and listen to a stretch where the voice starts. Watch **Gain reduction** in
   the Dynamics window: it shows how much the music is turned down.

If the music is lowered too little, lower the **Threshold** or raise the **Ratio**. If it
also drops when it should not, raise the **Threshold**.

The voice itself is not changed and does not need to be on the same bus. Mute or solo the
voice and you hear what the key hears: the key follows what you hear from the track, after
its fader.

## Tips

- **Key low cut**: set it to about 150 Hz so that a bump or a hum in the voice track does not
  duck the music.
- Use **Gate / Expander** on the voice track to cut the noise between words, before it keys
  the music.
- For speech that is sometimes quiet and sometimes loud, add a **Dialogue Leveler** to the
  voice track (the **+ FX** list of the strip) first. The ducking is then even.
- Also in the **+ FX** list: **De-Esser**, for harsh s sounds.

## If something goes wrong

- **Nothing happens.** The **Compressor** is not ticked, or **Key** is still **Self**, or the
  voice is quieter than the **Threshold**.
- **The music drops all the time.** The voice track has noise or breathing above the
  threshold. Raise the **Threshold**, or put a **Gate** on the voice track.
- **The music pumps.** Make the **Release** longer.
- **The source I want is grey in the list.** It would make a loop: a track cannot take its
  key from itself, from a bus it feeds, or from a strip that takes its key from this one.
