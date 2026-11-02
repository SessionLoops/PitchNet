# Track-mode analysis queue

These are manual DAW regression checks; they have not been run by the coding agent.

1. Create a track with three independent, previously unanalyzed ARA regions. Select the middle region and open PitchNet in Track mode. Verify one popup stays open through all three analyses, the overall percentage advances, the selected region stays selected, and every region gains visible pitch/note data.
2. Split one region into several linked regions. Reopen the editor. Verify the shared modification is analyzed only once and all its windows display the corresponding content.
3. Edit pitches, save the host project, close and reopen it. Verify saved edits are restored unchanged and already analyzed modifications do not run pitch detection again.
4. Verify the popup has no Cancel button, retains its original 399 × 98 size, and keeps the label and progress bar in their original positions. Close the editor during the second analysis, then reopen it. Verify the first result is retained and only missing analyses resume.
5. Close the editor during analysis, then reopen it. Verify no stale completion changes selection or accesses the old editor, and completed results remain available.
6. While analyzing, use the host to select another region or delete the region being processed. Verify the old job cannot publish into the new selection or deleted modification. Delete an unrelated region and verify the current job continues.
7. Open in Clip mode. Verify only the selected region is analyzed. Switch to Track mode and verify missing analyses are queued only for the selected region's track.
8. Simulate unavailable source samples or a failed source read. Verify unavailable sources are retried when ready, read/analysis failures do not loop forever, and other regions still complete.
9. Start a pitch render, then switch to Track mode. Verify queued analysis waits for synthesis to finish.

Overall progress covers distinct modifications requiring analysis, so multiple linked playback regions can correspond to one job. The popup keeps its original label without a job-count suffix. Source reads retain the existing ARA host/message-thread path; pitch analysis runs on the loader worker.
