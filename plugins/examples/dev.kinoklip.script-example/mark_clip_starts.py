# Marks the first frame of every clip of the timeline with a marker.
#
# A script is a normal python3 program. `kinoklip` is the bridge module Kinoklip provides: every
# kinoklip.call("<command>", ...) runs one command of Kinoklip's command layer (the same ones as
# `kinoklip --help` lists and the MCP server offers) on the open project. Reading needs the
# permission "project.read", changing needs "project.edit" (see plugin.json).
import kinoklip

clips = kinoklip.call("clip.list")["clips"]
seen = set()
for clip in clips:
    if clip["start"] in seen:
        continue  # a video and its audio start together: one marker is enough
    seen.add(clip["start"])
    kinoklip.call("marker.add", at=clip["start"], name=clip["name"] or "Clip", color="blue")
kinoklip.log("Added %d marker(s)" % len(seen))
