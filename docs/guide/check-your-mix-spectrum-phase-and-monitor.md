# Check your mix: spectrum, phase and monitor controls

You will look at the sound of your mix (which frequencies are loud, whether left and right
cancel each other) and change what you hear in your speakers without changing the video you
render.

## See the spectrum

1. Open the **Audio** page and play the timeline. The **Analyzer** panel is next to the
   meters at the top.
2. The bars show the frequencies from low (left, 20 Hz) to high (right, 20 kHz) and how loud
   each one is (from -90 to 0 dB). A thin line above the bars is the **peak hold**: it keeps the
   highest level of each frequency for a moment.
3. The list above the bars chooses what you look at: the **Main mix** or one track or bus on
   its own (a track is shown while the timeline plays at normal speed).
4. **Peak hold** switches the thin line on and off. **Slope** tilts the picture upwards to the
   right (3 dB per octave makes pink noise flat, 4.5 dB per octave suits a full mix), so music
   does not look like a hill that falls to the right. **Smoothing** chooses how much the bars lag
   behind the sound: Fast, Slow, or none.

The choices are remembered for next time.

## See the phase

Under the spectrum, the **correlation** meter shows how much the left and right sides are
alike: **+1** is the same sound on both sides (mono), **0** is wide stereo, **-1** is out of
phase. A value that dips below 0 for long means parts of your mix would vanish on a mono
speaker or a phone. The round picture beside it (the **goniometer**) shows the same thing as a
shape: a vertical line is mono, a wide cloud is stereo, a horizontal line is out of phase.

## Change what you hear: Monitor

The **Monitor** buttons change only the sound in your speakers. The meters, the render and the
export do not change.

- **Dim** lowers what you hear by the amount in the box next to it (-20 dB to start with).
- **Mono** plays left and right added together, to check how your mix sounds on a mono speaker.
- **Mute** silences your speakers. It is off again the next time Kinoklip starts.

Dim, Mono and the amount are remembered for you, not saved in the project.
