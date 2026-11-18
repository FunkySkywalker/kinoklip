# Change the frame rate of a timeline

You will change a timeline from, for example, 24 to 60 frames per second after the clips are
already on it, so that your 60 fps footage plays smoothly and keyframes have more frames to
move between. Nothing moves in time: every clip, marker, fade, transition, effect and keyframe
stays at the same second.

## Change it for one timeline

1. Open the timeline, then choose **Timeline > Timeline Settings**.
2. If **Use project settings** is ticked, untick it. The settings below it become editable.
3. Choose the new rate in **Timeline frame rate**. A note under the box says what will happen.
4. Click **OK** and confirm **Change Frame Rate**.

The project keeps its own frame rate; only this timeline (and the compound clips inside it that
ran at its old rate) changes. **Edit > Undo** takes the whole change back in one step.

## Change it for the whole project

1. Choose **File > Project Settings**.
2. Pick the new **Timeline frame rate**. When timelines that use the project settings have clips,
   a note under the box tells you they will be converted.
3. Click **OK** and confirm **Change Frame Rate**. One **Undo** step undoes it.

## What to expect

- A clip's start or end can move by at most half a new frame, because it has to land on a whole
  frame. Clips that touched still touch, and gaps stay gaps.
- Going to a higher rate loses nothing. Going to a lower rate (60 to 24) can merge two keyframes
  that land on the same frame; the one nearer its original moment is kept.
- Source in and out points are times, so every clip still shows the same part of its media.
- Effect settings counted in frames, such as the spacing of Motion Trails, are converted too.
- The timeline view keeps its zoom per second, and the playhead stays at the same time.

## Make your settings the default for new projects

If every video you edit is, say, 1440p at 60 fps, set it up once so each new project starts that
way instead of at 1920 x 1080 and 24 fps.

1. Open a project and choose **File > Project Settings**.
2. Set the **Timeline resolution** and **Timeline frame rate** you always use (and any other
   setting you want, such as the standard transition).
3. Click **Presets** in the list on the left, then **Save As User Default Config**, and confirm.
4. Click **OK** (or Cancel; the default is already saved).

From now on every new project (Project Manager, **File > New Project**, and the `project new`
command) starts from those settings. To go back to the built-in 1920 x 1080 at 24 fps, click
**Reset to factory default** on the same page. Projects you already have are not changed.

The **Presets** page also keeps named presets for different kinds of work. **Save As...** stores
the settings under a name, **Load** applies a preset to the open project, **Update** replaces a
preset with the settings now entered, **Delete** removes one, and **Import...** and **Export...**
read and write a preset as a file you can send to someone. **Load** keeps the frame rate of a
project whose timeline already has clips, and says so under the list.

When you add your first clip to a project whose frame rate and picture size differ from the
clip's, Kinoklip asks once whether to change both to match.

To do the same from a terminal on a saved project file, see
[Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
