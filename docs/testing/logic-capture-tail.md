# Logic non-ARA capture validation

## Change

The AU reports an infinite tail while capture is armed or recording, and zero
otherwise. Arm/disarm transitions notify the AU host that TailTime changed.
Automatic disarm uses a one-shot message-thread notification, not a timer.
Other formats still report zero. Capture finalizes on the real stopped callback
(or the existing maximum capture length), not on silence or elapsed time.

The JUCE submodule has a local extension: ChangeDetails gains a tail-length flag
and the AU wrapper translates it into a TailTime property notification. Retain
these submodule changes along with the processor changes.

## Observed in Logic, 2026-09-10

The fixed two-second tail failed at the end of the file. The manual-disarm trace
showed 653 callbacks, zero stopped callbacks, 668672 captured samples, and a
90726 ms callback age while still capturing.

With the dynamic infinite tail, the user captured the beginning and then the
end of the file and reported the analysis popup appearing. The corresponding
session log, debug_20260910_182854.log, showed:

| Capture | Armed tail | Callbacks | Stopped callbacks | Captured samples | Tail at analysis dispatch |
| --- | --- | --- | --- | --- | --- |
| Beginning | infinity | 1062 | 1 | 1086464 | 0 |
| End | infinity | 550 | 1 | 562176 | 0 |

Analysis dispatched at 18:29:23 and 18:29:46 respectively. These observations
support the fix for the reproduced case; they do not establish behavior across
all Logic versions or configurations. Temporary tracing was removed afterward.

## Regression checks

Load the AU as an ordinary insert without ARA. Use captures longer than 0.5
seconds, with the editor open and bypass off.

1. Stop inside a region; analysis should open.
2. Play beyond a region, wait 5–10 seconds, then stop; analysis should open.
3. Capture two regions separated by at least five seconds; capture must not
   finalize in the gap, and both regions must retain their timeline alignment.
4. Repeat with the track selected/unselected and input monitoring on/off.
5. Arm and manually disarm without playback; confirm the AU tail returns to zero.
