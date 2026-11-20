# UI audit, November 2026: Kinoklip look against Resolve 21

Reference: the Resolve 21 screenshots of the Cut, Edit, Color, Fairlight and Fusion pages
(1440x810, scaled to the 1920x1080 layout for every comparison, so "px" below means pixels at
1080p). Ours: `kinoklip --widget-style <style> --color-scheme <scheme> --screenshot <page>`
at 1920x1080. Method: the same region of both pictures cropped and enlarged side by side,
colours averaged over 3x3 pixels. The matrix checked: widget styles Kinoklip, Breeze, Fusion
(Libadwaita is not on this branch) times colour schemes Kinoklip, Breeze Dark, Breeze Light,
all seven pages, the Audio page's popups, Preferences and Project Settings.

## Findings

| Area | Resolve 21 | Ours before | Ours now |
|---|---|---|---|
| Track header, arrangement | two lines: patch, lock, auto select, enable (video) or S and M (audio) on the first, the track name bold on the second; no clip count | name beside the patch, buttons right-aligned, clip count on the second line | patch and buttons cluster on the first line in Resolve's order (lock, auto select, enable or solo and mute, then Kinoklip's sync lock), the name bold on a line of its own; rows under 44 px and the Audio page's two-line audio headers keep their old arrangement |
| Header buttons, state | flat dark plates 16 px, state shown by the glyph (bright = on); locked plate red | solid blue (video) or green (audio) plates behind chunky white bars for sync lock and auto select | plate 18 px in the panel colour, a little lighter when on; glyph bright when on, dim when off; locked, muted, soloed keep the scheme's red and yellow |
| Sync lock glyph | (no such button in Resolve) | two flat bars joined by a bar: looked like an equals sign on a blue plate | two arrows chasing round a circle, painted with the scheme text colour |
| Auto select glyph | `<\|>` chevrons round a capped bar | two flat bars (identical to the sync lock's) | `<\|>` painted like Resolve's |
| Enable (video) glyph | film strip | eye icon | film strip, painted; dim when the track is disabled |
| Mute and solo | the letters M and S | speaker and headphone icons | the letters, bold, bright when on |
| Destination patch | dark red plate, red outline | track colour (blue or green) when targeted | red plate and outline from the scheme's negative colour when targeted (patch is 30x18 against Resolve's about 25x17, not changed: it is the click target) |
| Arm (record) button | red | two fixed red literals | the scheme's negative colour (the one hard-coded colour in the track headers) |
| Track name font | 9 pt bold | regular small | semi-bold small (the shared SmallEmphasis role) |
| Timeline ruler and timecode corner | one colour with the lanes, #29282e | ruler #242229, corner darker again | Kinoklip scheme: the window colour for both (#28262e); other dark schemes unchanged |
| Combo box lists | wide enough for the longest entry | as wide as the combo box itself: "Narration" cut to "Narrat" in the Audio page's Analyzer "Main mix" combo; 160 px list for a 183 px entry in the Kinoklip style, 60 px for 148 in Breeze | every combo list is widened to its widest entry before it is shown, in every style (shifted left at a screen edge); test fails without it in four styles |
| Combo list and menu heights | as tall as the entries | checked for 1, 3 and 8 entries in every style, and the Audio page's FX menu (17 entries), bus menu, automation and meter combos: no list or menu is taller than its entries plus a 24 px frame; an oversized dropdown could not be reproduced here, the defect found was the clipped width above | test added and passing |
| Effects library sidebar | n/a | "Video Transitions" and the other category names hard-cut at the sidebar edge (Breeze: a horizontal scroll bar) | elided with "..." and the full name as tooltip |
| Mixer FX and bus slots | dark bordered wells | bare text with a chevron, next to a bordered "Read" combo box | framed like the combo box under them (Kinoklip style only; other styles draw their own tool buttons) |
| Page bar inactive labels | dim grey (about #6a6a6a) | 90 % of the text colour | not changed: an earlier readability decision (comment in KinoklipStyle) |
| Panel colours (Kinoklip scheme) | media pool #212026, inspector #25262b, lanes #29282e, page bar #17181a | #1b1a1e, #28262e, #28272c, #16171a | not changed: within 6 levels; the scheme was measured against earlier shots |

## Things that still differ, on purpose or for later

- Resolve's Inspector tab strip is icons above labels (six tabs including Image and File);
  ours is text tabs with the red underline. Layout change, not a style one.
- Resolve's Cut page, mixer strips (Input, Track FX, Order, Effects, Dynamics, EQ rows) and
  toolbar icons are a different design; matching them is page work, not theme work.
- Resolve has no sync lock button; ours stays (sync locks, 1.8) but is now drawn quietly.
- The destination patch is one button; Resolve has a source patch (teal) and a destination
  patch (red) per track. Adding the source patch is editing logic, not look.
- The offscreen platform has an 800x600 screen: popup pictures in `local/ui-audit/after` are
  pushed left by Qt's screen clamp, which does not happen on a real desktop.

## Checked and fine

Hard-coded colours in `src/ui` (about 125 literals): all are deliberate (colour wheels, scopes,
overlays over video, user colour labels, Vim mode colours); the track header's were the only
ones that should have followed the scheme. Preferences and Project Settings look consistent in
all styles. Fusion style and Breeze keep their own control drawing; the Kinoklip-only changes
are guarded by the style check.

## Tools added

`kinoklip --screenshot popup:<page>:<object name>[:<n>]` opens the n-th tool button menu or
combo list of that object name (`@combo` = any combo box) on a page and grabs it with the
popup; `KK_SCREENSHOT_SUBMENU=<n>` also opens a submenu. Tests: `popupsAreNoTallerThanTheirItems`,
`comboListsFitTheirEntries` (tests/app/test_theme.cpp), `nameLineSitsUnderTheButtons`
(tests/timeline/test_timelinelayout.cpp).
