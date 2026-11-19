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

Buses have no slots yet.
