# Line up clips by their sound (Auto Align Clips)

You filmed with a camera and recorded the sound on a separate recorder. Both clips are on the
timeline, but they start at different moments. **Auto Align Clips** moves the recording so its
sound matches the sound the camera heard. You do not have to line up the waveforms by eye.

## Line up an audio recording with a video clip

1. Put the video clip on a video track and the audio recording on an audio track, roughly where
   they belong.
2. Select both clips: click one, **Ctrl**-click the other.
3. Right-click one of them and choose **Auto Align Clips > Based on Waveform**.

The **reference** is the selected clip on the lowest video track, here the video clip. It never
moves. The other clips move (together with the clips linked to them, so a video clip keeps its
own sound) until their sound matches the reference. If you select only audio clips, the first
one you selected is the reference.

Kinoklip compares the sound of the files, not only the part you see on the timeline, so the
recording can start earlier or later than the camera clip, and be louder, quieter or noisy. The
camera must have heard the same sound as the recorder (speech, music, a clap).

Everything is **one undo step** (Ctrl+Z puts the clips back).

## When Kinoklip does not move a clip

A message in the status bar tells you why. A clip stays where it is when:

- **no clear match was found**: the sounds have too little in common, or the sound repeats so
  evenly (a steady hum, a metronome) that more than one position fits. Kinoklip never guesses;
- the move would **overwrite another clip** on its track: make room or move the other clip first;
- a track of the clip is **locked**, or the clip would start **before the beginning** of the
  timeline (move everything later first);
- the clip has no sound, the media is offline, or the clip is retimed (a speed change).

The other selected clips are still aligned.

## Based on Timecode

**Auto Align Clips > Based on Timecode** uses the start timecode stored in the files
(cameras that share a timecode, or recorders jam-synced to the camera). It is instant and needs
no sound. Files without a start timecode are left alone.

## From the command line or an AI assistant

The same thing is the `clip align` command (the `clip_align` tool of the assistant server):

    kinoklip clip align --project p.kkproj --reference 3 --clip 7 --method waveform

`--reference` is the clip that stays (default: the `--clip` on the lowest video track), `--clip`
the clips to line up (repeat it or separate ids with commas), `--method` is `waveform` or
`timecode`, and `--mute-reference` silences the reference's own sound afterwards (for example
the camera's built-in microphone). The result lists every clip with its `status` (`aligned`,
`unchanged`, `unreliable`, `skipped` or `blocked`), the frames it moved and how sure the match
was; if no clip could be moved the command fails with `unreliable` or `cannot_align` and the
reasons. See [Control Kinoklip from the command line](control-kinoklip-from-the-command-line.md).
