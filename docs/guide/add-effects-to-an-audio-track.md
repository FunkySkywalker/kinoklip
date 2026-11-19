# Add effects to a whole audio track

You will put audio effects on a track instead of a single clip, so everything on that track
(every clip, now and later) gets the same treatment: a compressor on a voice track, an
equalizer on music, noise reduction on a whole interview.

## Add an effect to a track

1. Open the **Audio** page. Every audio track has a strip in the **Mixer**.
2. On the strip, under **EQ** and **Dyn**, click **+ FX** and choose an effect from the menu.
   Or open the **Effects Library** (the **Effects** button in the toolbar at the top), stay on
   **Audio FX**, and drag an effect onto the strip.
3. The effect appears in a slot on the strip. Click the dot to **bypass** it (the name turns
   italic) and click it again to switch it on. Click **x** to remove it.

Track effects work on the sound of the whole track after the clips and their own effects, and
before the track's EQ, Dyn, pan and fader. The slots run from top to bottom, so the order matters:
a Gain before a Limiter is not the same as a Limiter before a Gain. They play and render the
same, and Undo takes each change back.

## Change the controls

Double-click a slot. The **Inspector** opens on the right with the effect's controls. You can
also click the strip (not on a button) to see all of its effects in the Inspector: there you can
switch effects on and off, move them up and down, add more with **Add Effect**, and remove
them. Click a clip on the timeline to get the clip's own properties back.

## Reverb, Chorus, Flanger, Distortion and Stereo Width

These five work on a clip (Inspector, **Add Effect**) and in a track slot. The tail of a reverb
or an echo rings out after the end of the clip, over the clips that follow, and in the render;
a reverb in a **track slot** also rings out after the last clip.

- **Reverb**: **Pre-delay** (how long after the sound the room starts), **Decay** (seconds),
  Room Size, Damping (cuts the highs of the tail), Early Reflections, Width and Mix. Choose a
  **Preset** (Small Room, Large Room, Hall, Plate, Chamber, Cathedral) and its values appear in
  the sliders; move any slider and the Preset changes to **Custom**. Mix is not part of a
  preset.
- **Chorus**: Rate, Depth, Delay, Feedback, Voices (1 to 4), Stereo Spread and Mix.
- **Flanger**: Rate, Depth, Delay, Feedback (negative gives a hollow sound) and Mix.
- **Distortion**: Drive, Tone, a Mode (Soft Clip, Hard Clip, Bit Crush, Tube-like),
  **Oversampling** (Off, 2x, 4x: fewer harsh, out-of-tune overtones, a little more processing),
  Output and Mix. Lower the Output as you raise the Drive.
- **Stereo Width**: Width from 0 (mono) over 100 (unchanged) to 200, Mono Below (keeps the
  bass solid) and Output.

Mix at 0 leaves the sound untouched. Hover a control to see what it does.

Buses have no slots yet.
