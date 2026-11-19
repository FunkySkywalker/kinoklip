# Automate volume over time

Automation makes a setting change by itself while the video plays: the music dips under a
voice, a sound pans from one side to the other, an effect turns up toward the end. You draw
the change as a line (a **curve**) under the track, or you record it by moving a fader while
the video plays.

## Draw a volume curve

1. Open the **Audio** page (**Shift+7**).
2. In the header of the track, click the small curve button (**Automation**). A lane opens
   below the track. (On the Edit page, right-click the track header and choose **Show
   Automation Lane**.)
3. The list at the left of the lane says **Volume**. Click in the lane to add a point. Drag
   the point up or down to change the volume, and left or right to move it in time.
4. Add a second point further along. The volume changes smoothly between the two points.
5. Press **Play**. The fader of the track follows the curve.

More about the lane:

- Drag a **line** between two points up or down to move both points together.
- Click a point and press **Delete** to remove it. Every change is one undo step (**Ctrl+Z**).
- Right-click a point to change the line after it: **Linear**, **Smooth**, **Ease In**,
  **Ease Out**, **Ease In and Out** or **Hold (step)**.
- Without a curve the lane shows a dashed line: the value the fader has now.
- The curve belongs to the **track**. The volume line on a clip (its own level keys) stays as
  it is and is applied as well.

## Other things to automate

Open the list at the left of the lane to choose what the lane shows:

- **Volume** and **Pan** of the track.
- **Mute**: a step line; at 1 the track is silent. A dot in the list marks the parameters
  that already have a curve.
- **Send to ...**: the level of each send of the track.
- Every parameter of the **effects** on the track (the **+ FX** list of the strip), for
  example the gain of an equalizer band.

## Record a fader move while playing

Above the timeline and the mixer is the **Automation** bar. Click a mode to set it for all
tracks (or choose the mode of one track in its strip):

- **Off**: the curves are ignored; the fader keeps the value you set.
- **Read**: the curves play (the normal mode).
- **Touch**: while the video plays, hold the fader and move it; your moves are recorded. When
  you let go, the fader returns to the curve that was there before.
- **Latch**: like Touch, but after you let go the last value keeps being recorded until you
  stop playing.
- **Write**: the fader value is recorded all the time while the video plays, and replaces
  what was there.
- **Trim**: you move the fader a little, and the change is added on top of the existing
  curve, so a recorded ride keeps its shape but gets louder or quieter.

Every pass is one undo step.

## Tips

- To lower the music under a speaker, use Touch: play, hold the music fader down when the
  speaker starts and let go when he stops.
- A recorded curve with too many points is easy to clean: set the mode to **Read**, open
  the lane and delete the points you do not need.
- The render uses exactly the same curves as playback.

## If something goes wrong

- **The fader does not follow the curve.** The mode is **Off**. Choose **Read**.
- **Moving the fader changes nothing in the lane.** Recording only works while the video
  plays, in **Touch**, **Latch**, **Write** or **Trim**. When it is stopped, a move of a
  fader that has a curve changes the point at the playhead.
- **I cannot find the parameter.** Effects and sends must exist on the track first; they then
  appear in the list.
