# Use AgX to tame bright highlights

Footage with bright skies, lamps or reflections often **clips**: the brightest parts turn flat
white and the colours there go wrong. **AgX** is a tone mapping that rolls the bright parts off
smoothly, so highlights keep a little colour and detail. It is the default "view transform" of
Blender, and many people prefer it to ACES. Kinoklip has it built in, in two places:

- the **AgX palette** on the Color page, for one node (one clip or one part of a clip), and
- **Project Settings > Color Management**, for the whole timeline, in the viewer and in every
  render.

AgX changes how the picture looks, so judge it by eye. It works best on footage with plenty of
highlight detail, and it makes the picture a little flatter and softer; add contrast with a
look or with the other Color page tools.

## AgX on one clip

1. Open the Color page (**Shift+6**) and select the node that should get AgX. On a clip
   without a grade there is no node yet; AgX then makes node **01**.
2. Click **AgX** in the bar above the palettes and tick **Enable AgX**.
3. Choose the **Input**: what the picture's numbers mean. **Rec.709 (gamma 2.4)** is right for
   ordinary video from most cameras, phones and screen recordings, and is the default. Use
   **sRGB** for pictures from the web, **Linear** for linear footage, or the camera's log
   profile (Apple Log, S-Log3, V-Log, LogC3) for log footage. Kinoklip only undoes the log
   curve; it does not change the camera's colour range.
4. Move **Exposure** to brighten or darken before the tone mapping. One stop doubles or halves
   the light.
5. Pick a **Look** if you want one: **Punchy** (more contrast and colour), **Golden** (warm) or
   **Greyscale**. **None** is plain AgX.
6. Leave **Output** on **Rec.709 (gamma 2.4)** for video, or choose **sRGB** for pictures.

AgX is the last step inside its node, after the node's other corrections. To grade after AgX,
add another node after it (**Alt+S**). **Reset** in the palette bar turns AgX off again.

## AgX on the whole timeline

1. Choose **File > Project Settings** and open **Color Management**.
2. Set **Output tone mapping** to **AgX**, and choose an **AgX look** if you like.
3. Save. The viewer and every render now show the timeline through AgX, after all grades and
   before captions.

The default is **None**, which leaves every picture exactly as it was. The setting belongs to
the project: projects saved before it existed open with None.

Use the node for one clip and the project setting for the whole timeline, not both on the same
clip: the picture would be tone mapped twice.
