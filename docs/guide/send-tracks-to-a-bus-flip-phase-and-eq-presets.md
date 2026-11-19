# Send tracks to a bus, flip the phase and use EQ presets

Three small tools on the **Audio** page strips: **sends** copy a track's sound into a bus (for
a shared reverb, or a submix of several tracks), **Phase Invert** flips the polarity of a track,
and the **EQ presets** set the equalizer in one click.

## Send a track to a bus

1. Open the **Audio** page (**Shift+7**). Make a bus for the shared sound: **Add Bus** in the mixer,
   then right-click its name plate and tick **Aux Bus (send only)**. An aux bus gets its sound
   only from sends; you cannot route a track to it with the bus menu.
2. On the strip of the voice, click **Sends**. In the window choose the bus and click
   **Add Send**.
3. Set the **level** (dB) of the send. **Post** sends the sound after the fader, so the
   send follows the fader; **Pre** sends it before the fader and the pan, so moving the fader
   does not change the send. **Pan** moves the send to the left or the right.
4. Send the aux bus on to the main bus: click **Sends** on the aux strip and add the main bus.
   The aux bus has its own EQ, dynamics, pan and fader, so you can shape the combined sound of
   everything sent to it. (Buses have no effect slots yet.)

A bus can send to another bus in the same way. Kinoklip does not offer a bus that would make a
circle (a bus that feeds itself through other buses): it is simply not in the list.
Only the **Output** bus is heard, so the aux bus must feed it, directly or through other buses.

Every bus also has a **pan** knob now. Every change is one undo step, and playback and
rendering give the same sound.

## Flip the phase

Click the **Ø** button on a strip to invert the polarity of the whole strip (both sides).
Right-click it to flip only the left or only the right side. Use it when two microphones on
the same voice cancel each other and make it thin. The button is lit while it is on.

## Use an EQ preset

1. Click **EQ** on a strip to open the equalizer.
2. Click **Presets** and choose one: **Flat** (switches the equalizer off), **Voice**, **Music**,
   **Telephone**, **Rumble Cut**, **De-Mud** or **Podcast Voice**. All six bands change.
3. To keep your own settings, adjust the bands, then **Presets > Save as Preset...** and type a
   name. Your presets are in the same menu; **Delete Preset** removes one.

Choosing a preset is one undo step.
