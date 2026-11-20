# Bug hunt of 1.12: Edit and Cut page timeline

Round of 2026-11-21 over the typical editing workflows: blade, ripple, roll, slip, slide, linked
audio, transitions, markers, speed and duration, freeze frame, swap, move, delete and ripple
delete, each with undo and redo. The fixes and checks are spec section 145.

## How it was searched

`tests/timeline/test_bughunt13edit.cpp` drives the workflows through the controller (actions and
commands) and the widget (mouse drags in the Selection, Trim and Blade tools). After every step
it checks that the timeline is valid, that the step is exactly one undo step, that undo restores
the saved timeline JSON exactly and that redo brings the same result back. It reports every
problem of a workflow at once. `tests/timeline/test_rippleaudit.cpp` does the same for the ripple
entry points and sync lock (spec 145.1).

## Found and fixed

- Nothing in the workflows of the two tests broke: no invalid timeline, no step with two undo
  steps, no undo that did not restore the project. The ripple audit found no entry point out of
  sync.
- Voice-over count-in drifted when the event loop was busy: it counted timer ticks, so a late
  tick pushed the start of the take later and later (2906 ms for a 2 s count-in under load). It
  now follows a clock (`VoiceOverController::tickCountIn`); test countInFollowsTheClockWhenTheEventLoopIsBusy.
- A Custom Tool shader that fails to compile now logs why (it was dropped silently).

## Not fixed

- `test_compcolor customToolFormulasAgreeOnBothRenderers` and `test_dynamiczoom` still fail now
  and then under heavy parallel load (about 1 run in 3 with 30 runs at once, never alone): the
  GPU picture is `rgba(0,0,0,0)` and stays so for every retry in that process, also from a new
  Compositor. The picture is not a null image, the shader compiles and `glGetError` is clean, so
  it is not a failed readback or compile. Adding any output to the render path hid it. The failure
  message of the compcolor test now names the renderer.
