# Remove background noise from a clip

You will take hiss, hum or room noise out of the sound of a clip with the **Noise Reduction**
audio effect. It works on a voice recorded with a camera or a phone, and it runs on this computer:
nothing is uploaded or downloaded.

## Clean a voice in one step

1. Select the clip with the sound. Open the **Inspector**, click the **Effects** tab, then
   **Add Effect** and choose **Noise Reduction** (it is in the audio effects; you can also drag
   it from the **Effects Library** onto the clip).
2. Leave **Mode** on **Auto Speech** and **Model** on **RNNoise (AI, optional)**. RNNoise is a
   small open neural network made for speech. It is part of Kinoklip, so there is nothing to
   install.
3. Play the clip. Use **Reduction** (dB) to say how much quieter the noise may become, and
   **Dry / Wet** to blend the original back in.

Choose **Spectral Gate** as the Model if the sound is not speech or you do not want the neural
network: it works out the noise by itself from the quietest parts of the clip.

## Teach it the noise (Manual Learn)

1. Find a pause where only the noise is heard. Mark it on the timeline with **I** and **O**
   (In and Out). Without marks the first second of the clip is used.
2. Press **Learn** in the effect. The mode changes to **Manual Learn** and the noise is taken
   out of the whole clip.
3. **Clear Profile** forgets what was learned. The profile is saved with the project.

## The controls

- **Threshold** and **Ratio**: sound quieter than the Threshold is treated as noise only and made
  quieter by the Ratio (never more than **Reduction**). The default Threshold of -70 dB leaves
  normal recordings alone; raise it to silence the pauses between words.
- **Attack**: how fast the reduction lets go when sound returns. Short values keep word starts
  sharp, long ones are smoother.
- **Sensitivity**: higher removes more noise, but can make the voice thin or watery.
- **Reduction**: the most the noise is made quieter, in dB.
- **Dry / Wet**: 0 is the original sound, 100 the cleaned sound.
- **Output Gain**: the level of the result.

## Good to know

- The cleaned sound is about 20 ms later than the original (it is cut into short blocks). Clips
  keep their place; only the sound inside the clip is shifted by that tiny amount, in playback and
  in the render alike. With **Dry / Wet** at 0 there is no delay at all.
- RNNoise is made for speech and for the noise of rooms, fans and hiss. Loud pure white noise is
  one thing it removes poorly; try the spectral gate or Manual Learn for that.
- Very strong settings can make a voice sound underwater. Lower **Sensitivity** or
  **Reduction** a little.
