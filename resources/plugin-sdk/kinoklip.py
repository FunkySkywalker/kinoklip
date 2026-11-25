# SPDX-License-Identifier: GPL-3.0-or-later
"""Kinoklip script bridge.

A Kinoklip plugin script runs as a separate `python3` process. This module is the whole
interface: it sends command-layer calls (the same commands as `kinoklip <command>` on the
command line and as the MCP tools) to Kinoklip and returns their results.

    import kinoklip

    for clip in kinoklip.call("clip.list")["clips"]:
        kinoklip.log(clip["id"], clip["name"])

    kinoklip.call("effect.add", clip=12, type="gaussian_blur", set=["radius=8"])

`call(command, **arguments)` returns the result dictionary of the command or raises
`kinoklip.CommandError` (with `.code` and `.message`). Argument names with a dash
("source-in") cannot be Python keywords: pass them as a dictionary,
`call("clip.trim", {"clip": 12, "source-in": "00:00:01:00"})`.

What a script may do is limited by the permissions in its plugin.json: reading commands
need "project.read", commands that change the project need "project.edit"; everything a
script changes is one undo step in the window.

Nothing else is needed: print() and kinoklip.log() show in the Scripts output.
"""

import json
import sys

__all__ = ["call", "log", "CommandError", "commands"]

_PREFIX = "@kk "
_next_id = 0


class CommandError(Exception):
    """A command failed. `code` is the stable error code (e.g. "unknown_clip")."""

    def __init__(self, code, message):
        super().__init__("%s: %s" % (code, message))
        self.code = code
        self.message = message


def call(command, _arguments=None, **arguments):
    """Runs one command and returns its result dictionary (raises CommandError on failure)."""
    global _next_id
    if _arguments:
        arguments = dict(_arguments, **arguments)
    _next_id += 1
    request = {"id": _next_id, "command": command, "args": arguments}
    sys.stdout.write(_PREFIX + json.dumps(request) + "\n")
    sys.stdout.flush()
    line = sys.stdin.readline()
    if not line:
        raise CommandError("bridge_closed", "Kinoklip closed the connection")
    reply = json.loads(line)
    if not reply.get("ok"):
        error = reply.get("error") or {}
        raise CommandError(error.get("code", "error"), error.get("message", "command failed"))
    return reply.get("result", {})


def log(*parts):
    """Prints a line to the Scripts output."""
    print(*parts)
    sys.stdout.flush()


def commands():
    """The names of all commands a script can call."""
    return [c["name"] for c in call("kk.commands").get("commands", [])]
