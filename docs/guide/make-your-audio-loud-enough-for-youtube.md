# Make your audio loud enough for YouTube

Videos on YouTube, podcasts and TV are all played at a similar loudness. If yours is much
quieter, people turn it up; if it is much louder, the service turns it down. Kinoklip can
measure how loud your video is and bring it to the right level in one step.

Loudness is measured in **LUFS** (the louder, the closer to 0). The usual targets:

| Where it plays | Target |
|---|---|
| YouTube and other streaming sites | **-14 LUFS** |
| Podcasts | **-16 LUFS** |
| European TV (EBU R128) | **-23 LUFS** |
| US TV (ATSC A/85) | **-24 LKFS** |

## See how loud it is

1. Open the **Audio** page (**Shift+7**).
2. In the **Meters** panel, open the standard list and choose **YouTube / streaming (-14 LUFS)**.
   The line in the bars and in the graph moves to that target.
3. Press **Play**. **Short term** shows the loudness of the last 3 seconds, **Integrated** the
   loudness of everything played so far, **Loudness range** how much it changes, and
   **True peak max** the highest peak. Next to **Target** the meter says **Over by** or
   **Under by** a number of LU, or **On target** when you are within 1 LU.
4. The graph shows the short-term loudness of the last minutes against the target line.
   **Pause** stops measuring (the readings stay), **Start** goes on, and **Reset Loudness
   Meter** (the title bar of the panel) starts again from zero.

Play the whole video once for a right reading of **Integrated**.

## Fix it in one step

1. Right-click the name of the **main bus** in the mixer (or of a single track, in the mixer or
   in its timeline header) and choose **Normalize Audio Levels…**.
2. Keep **ITU-R BS.1770-4 (loudness)**, choose the **Standard** (for example **YouTube /
   streaming**), and click **Normalize**.
3. Kinoklip plays the whole mix silently, measures it (the In to Out range if you set marks,
   otherwise the whole timeline) and moves the fader by the right amount. **Ctrl+Z** undoes it.

Normalizing the main bus changes the whole programme; normalizing a track, for example a music
track, changes only that track.

## Let the render do it

Instead (or as well), the render can set the loudness of the file:

1. Open the **Deliver** page, tab **Audio**.
2. Tick **Normalize audio to** and choose the standard. The target and the **True peak limit**
   are shown; with **Custom** you can type your own numbers.
3. Render. Kinoklip measures the whole mix first, then renders the file about 0.5 LU from the
   target. If a peak would be above the true peak limit, a look-ahead limiter takes it down,
   so the sound does not distort, and Kinoklip measures the limited result and corrects the
   gain so the loudness still lands on the target. If the target cannot be reached under the
   limit, or needs heavy limiting, the job in the render queue says so ("loudness target not
   reached" or "heavy limiting"; hover it for the numbers).

## Tips

- Normalize the voice track first, then lower the music under it; see
  [Fade audio in and out, and add music](fade-audio-and-add-music.md).
- To make single clips sound alike, use [Make quiet and loud clips the same
  volume](even-out-clip-volumes.md).

## If something goes wrong

- **"There is no sound in this range."** The track or the mix is silent between the marks, or
  the track is muted. Check **In** and **Out** and the mute buttons.
- **The result is a little under the target.** The fader stops at +12 dB. A very quiet
  recording needs more gain on the clips first (**Volume** in the Inspector).
- **The meter and the render differ by a little.** The meter measures what you play; the
  render measures the whole file. Trust the render.
