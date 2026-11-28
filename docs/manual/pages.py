# Content of the PitchNet Reference Manual.
# Each page body is an HTML fragment. [[fig:file|caption]] inserts a screenshot
# from images/ when that file exists.

VERSION = "0.8.1"

def ico(name, alt=""):
    return f'<span class="ico"><img src="images/icons/btn-{name}.png" alt="{alt}"></span>'

# Numbered regions of the main window (x, y in % of images/main-window.jpg).
WINDOW_REGIONS = [
    ("Transport Toolbar", "toolbar.html#transport", 21.5, 3.6),
    ("Edit Tools", "edit-tools.html", 49.5, 3.6),
    ("Scale Selector", "correct-pitch.html#scale", 76.6, 3.6),
    ("Correct Pitch Macro", "correct-pitch.html#macro", 83.7, 3.6),
    ("Live Audition", "toolbar.html#audition", 87.3, 3.6),
    ("Undo / Redo", "toolbar.html#undo", 92.5, 3.6),
    ("Parameters Panel button", "parameters.html", 97.2, 3.6),
    ("Timeline", "piano-roll.html#timeline", 12.0, 14.7),
    ("Loop Bar", "piano-roll.html#loop", 40.0, 17.0),
    ("Piano Keys", "piano-roll.html#keys", 1.5, 40.0),
    ("Note Canvas", "piano-roll.html#canvas", 40.0, 55.0),
    ("Blob Height slider", "piano-roll.html#view-controls", 92.0, 70.5),
    ("Vertical Zoom slider", "piano-roll.html#view-controls", 92.0, 84.3),
    ("Vertical Auto-Zoom", "piano-roll.html#view-controls", 92.0, 91.9),
    ("Horizontal Zoom slider", "piano-roll.html#view-controls", 82.5, 96.2),
    ("Overview toggle", "piano-roll.html#overview", 97.2, 96.2),
    ("Vertical Scroll Bar", "piano-roll.html#view-controls", 99.4, 52.0),
]

def window_guide():
    pins = "".join(
        f'<a class="pin" href="{href}" style="left:{x}%;top:{y}%" title="{n}. {label}">{n}</a>'
        for n, (label, href, x, y) in enumerate(WINDOW_REGIONS, 1))
    legend = "".join(f'<li><a href="{href}">{label}</a></li>' for label, href, _, _ in WINDOW_REGIONS)
    return (f'<div class="guide"><img src="images/main-window.jpg" alt="The PitchNet main window">{pins}</div>'
            f'<p style="font-size:13.5px;color:#5d6470">Click a number to go to the page about that part of the window.</p>'
            f'<ol class="legend">{legend}</ol>')

PAGES = []
def page(slug, title, body):
    PAGES.append({"slug": slug, "title": title, "body": body})

# ---------------------------------------------------------------- Home
page("index", "PitchNet Reference Manual", f"""
<div class="hero"><img src="images/icon-128.png" alt="">
<div><h2>PitchNet {VERSION} Reference Manual</h2>
<p>PitchNet is a free, open-source pitch editor for vocals and instruments. It finds the notes in a recording,
shows them in a piano roll, and lets you change their pitch, timing, level and tone &mdash; with an instant preview
through a neural vocoder or a classic DSP engine. It runs as a standalone app and as a VST3, AU and AAX plug-in with ARA.</p></div></div>

<div class="quick">
<a href="getting-started.html">Getting Started</a>
<a href="tutorial-first-edit.html">Your first edit</a>
<a href="shortcuts.html">Keyboard shortcuts</a>
<a href="troubleshooting.html">FAQ &amp; Troubleshooting</a>
<a href="https://github.com/SessionLoops/PitchNet/issues">Report a problem</a>
<a href="search.html">Search the manual</a>
</div>

<h2>Guide to the PitchNet Window</h2>
<p>This is the PitchNet main window. Each numbered part links to its own page in this manual.</p>
{window_guide()}

<div class="note"><b>Help in the app:</b> hover over any button in PitchNet to see its name and shortcut.
The <a href="menus.html">menus</a> also show the shortcut for each command.</div>

<h2>PitchNet Reference Guide</h2>
<div class="cards">
<div class="card"><h3>Using PitchNet</h3><ul>
<li><a href="getting-started.html">Getting Started</a></li>
<li><a href="tutorial-first-edit.html">Tutorial: your first vocal edit</a></li>
<li><a href="projects.html">Opening, saving and exporting</a></li>
<li><a href="plugin.html">Using the plug-in (ARA and capture)</a></li></ul></div>
<div class="card"><h3>The PitchNet window</h3><ul>
<li><a href="menus.html">Menu Bar</a></li>
<li><a href="toolbar.html">Toolbar</a></li>
<li><a href="edit-tools.html">Edit Tools</a></li>
<li><a href="piano-roll.html">Piano Roll</a></li>
<li><a href="parameters.html">Parameters Panel</a></li>
<li><a href="tracks.html">Tracks Panel</a></li></ul></div>
<div class="card"><h3>Editing notes</h3><ul>
<li><a href="note-handles.html">Note Handles</a></li>
<li><a href="correct-pitch.html">Correct Pitch, Scales and Restore</a></li>
<li><a href="pitch-drawing.html">Drawing pitch curves</a></li>
<li><a href="timing.html">Timing edits</a></li>
<li><a href="breaths-sibilance.html">Breaths and sibilants</a></li></ul></div>
<div class="card"><h3>Analysis and sound</h3><ul>
<li><a href="analysis.html">How analysis works</a></li>
<li><a href="polyphonic.html">Polyphonic editing</a></li>
<li><a href="rendering.html">Rendering engines</a></li>
<li><a href="chord-track.html">Chord Track</a></li>
<li><a href="midi.html">MIDI reference and export</a></li></ul></div>
<div class="card"><h3>Customizing</h3><ul>
<li><a href="settings.html">Settings</a></li>
<li><a href="shortcuts.html">Keyboard shortcuts</a></li>
<li><a href="mouse.html">Mouse and trackpad</a></li>
<li><a href="languages.html">Languages</a></li></ul></div>
<div class="card"><h3>Index, Glossary and More</h3><ul>
<li><a href="glossary.html">Glossary of terms</a></li>
<li><a href="troubleshooting.html">FAQ and error messages</a></li>
<li><a href="building.html">Building from source</a></li>
<li><a href="credits.html">Credits and license</a></li></ul></div>
</div>
""")

# ---------------------------------------------------------------- Getting started
page("getting-started", "Getting Started", """
<p>This page covers installing PitchNet, what happens the first time you open a recording, and the basic
idea behind editing in PitchNet.</p>

<h2>Installing</h2>
<p>Download PitchNet from <a href="https://sessionloops.com/pitchnet#downloads">sessionloops.com/pitchnet</a>.
The download contains:</p>
<ul>
<li><b>PitchNet</b> &ndash; the standalone app.</li>
<li><b>PitchNet plug-in</b> &ndash; VST3 and AU (macOS), VST3 (Windows) and, in builds made with the AAX SDK, AAX. The plug-in supports
<a href="plugin.html">ARA</a> in hosts such as Studio One, Cubase and Logic Pro.</li>
</ul>
<p>The neural models PitchNet needs are installed with the app. Nothing has to be downloaded on first launch.</p>

<h3>System requirements</h3>
<table>
<tr><th>Platform</th><th>Notes</th></tr>
<tr><td>macOS</td><td>Apple Silicon or Intel. AI Resynthesis and analysis use CoreML (Neural Engine / GPU) when available.</td></tr>
<tr><td>Windows</td><td>64-bit. GPU acceleration through DirectML (most GPUs) or CUDA (NVIDIA builds). The CPU always works.</td></tr>
<tr><td>Linux</td><td>Build from source (<a href="building.html">see Building</a>). ALSA audio.</td></tr>
</table>

<h2>The basic idea</h2>
<p>PitchNet works in four steps, most of which happen on their own:</p>
<ol>
<li><b>Open</b> an audio file (WAV, MP3, FLAC, AIFF, OGG or M4A) with <kbd>Cmd</kbd>+<kbd>O</kbd>, or drag it onto the window.</li>
<li><b>Analysis</b> runs automatically. PitchNet detects the pitch of every frame, decides whether the input is a voice or
several instruments, and splits the audio into <a href="glossary.html#note">notes</a>. The notes appear as coloured
&ldquo;blobs&rdquo; in the <a href="piano-roll.html">piano roll</a>, with the detected pitch curve drawn through them.</li>
<li><b>Edit</b> the notes: drag them up or down, change their length and position, shape their pitch curve, or correct a whole passage at
once with <a href="correct-pitch.html">Correct Pitch</a>.</li>
<li><b>Listen and export.</b> Every edit is re-rendered in the background, so pressing <kbd>Space</kbd> plays the result straight away.
When you are happy, <a href="projects.html#export">export</a> the audio or the notes as MIDI, or save a project to keep working later.</li>
</ol>

<div class="tip"><b>Tip:</b> Nothing you do changes your original audio file. Edits live in the PitchNet project
(or in your DAW session when you use the plug-in), and every edit can be undone or restored per note.</div>

<h2>What next?</h2>
<ul>
<li>Follow the <a href="tutorial-first-edit.html">first-edit tutorial</a>.</li>
<li>Learn the window with the <a href="index.html">annotated window guide</a>.</li>
<li>Learn the <a href="shortcuts.html">keyboard shortcuts</a> &ndash; especially <kbd>1</kbd>&ndash;<kbd>4</kbd> for the edit tools.</li>
</ul>
""")

# ---------------------------------------------------------------- Tutorial
page("tutorial-first-edit", "Tutorial: Your First Vocal Edit", f"""
<p>This tutorial tunes a few notes of a sung phrase, adjusts one note's timing, and exports the result.
It takes about five minutes. Any dry (unprocessed) mono vocal recording works.</p>

<h2>1. Open the recording</h2>
<p>Choose <b>File &rsaquo; Open</b> (<kbd>Cmd</kbd>+<kbd>O</kbd>) and pick the file, or drag the file onto the PitchNet window.
A progress bar shows <i>Loading audio&hellip;</i>, <i>Analyzing audio&hellip;</i> and <i>Detecting notes&hellip;</i>.
When it finishes, the notes appear in the piano roll.</p>
[[fig:main-window.jpg|A vocal after analysis: each note is a blob, the white line is the detected pitch.]]

<h2>2. Look around</h2>
<ul>
<li>Press <kbd>Space</kbd> to play, <kbd>Space</kbd> again to pause. <kbd>Home</kbd> returns to the start.</li>
<li>Scroll with the mouse wheel or trackpad; hold <kbd>Shift</kbd> to scroll sideways.</li>
<li>Zoom with <kbd>Cmd</kbd> + scroll, or scroll over the <a href="piano-roll.html#keys">piano keys</a> (vertical) or the
<a href="piano-roll.html#timeline">timeline</a> (horizontal). Click the {ico('autozoom')} <b>Vertical Auto-Zoom</b> button to fit the notes.</li>
</ul>

<h2>3. Tune a note</h2>
<ol>
<li>Make sure the {ico('select')} <b>Main Tool</b> is active (press <kbd>1</kbd>).</li>
<li>Click a note that sounds flat or sharp. The <b>Note</b> and <b>Cent</b> fields in the toolbar show how far it is from the nearest semitone.</li>
<li>Double-click the note. It snaps to its pitch centre (the nearest semitone, or the nearest scale note &ndash; see
<a href="parameters.html#pitch">Double Click Snap</a>).</li>
<li>Or drag the note up or down. With <b>Drag Snap</b> on it moves in semitones; hold <kbd>Option</kbd> while dragging to move freely.</li>
</ol>

<h2>4. Tune a whole phrase</h2>
<ol>
<li>Drag a box around several notes in an empty part of the canvas, or press <kbd>Cmd</kbd>+<kbd>A</kbd> to select all.</li>
<li>Click {ico('quantize')} <b>Correct Pitch Macro</b> in the toolbar.</li>
<li>Set <b>Pitch Center</b> (how far each note moves toward its target, 0&ndash;100&nbsp;%) and <b>Pitch Drift</b> (how much of the slow
wandering inside each note is straightened). Turn on <b>Snap to Scale</b> to aim for the notes of the current scale. Click <b>OK</b>.</li>
</ol>

<h2>5. Fix the timing of one note</h2>
<ol>
<li>Press <kbd>4</kbd> for the {ico('timing')} <b>Timing Tool</b>.</li>
<li>Drag the start or end of a note to move it, or drag the boundary between two notes. Consonants keep their length; the vowel is stretched.</li>
</ol>

<h2>6. Compare and export</h2>
<ul>
<li>Hover over a note and click the {ico('reset')} <b>Restore Original</b> button to undo just that note's pitch, timing, formant or level edits.</li>
<li>Choose <b>File &rsaquo; Export Audio&hellip;</b> (<kbd>Cmd</kbd>+<kbd>E</kbd>) to write a WAV, FLAC, AIFF or OGG file.</li>
<li>Choose <b>File &rsaquo; Save Project</b> (<kbd>Cmd</kbd>+<kbd>S</kbd>) to keep your edits in a <code>.pitchnet</code> project.</li>
</ul>
""")

# ---------------------------------------------------------------- Menus
page("menus", "Menu Bar", """
<p>On macOS the menus are in the system menu bar; on Windows and Linux they are at the top of the window.
In the plug-in, the same commands are reached from the editor's toolbar and right-click menus.</p>

<h2 id="pitchnet">PitchNet menu (macOS)</h2>
<table><tr><th>Command</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>About PitchNet</td><td></td><td>Version, credits and third-party notices.</td></tr>
<tr><td>Settings&hellip;</td><td><kbd>Cmd</kbd>+<kbd>,</kbd></td><td>Opens <a href="settings.html">Settings</a>.</td></tr>
<tr><td>Quit PitchNet</td><td><kbd>Cmd</kbd>+<kbd>Q</kbd></td><td>Asks to save unsaved changes, then quits.</td></tr>
</table>

<h2 id="file">File menu</h2>
<table><tr><th>Command</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>Open</td><td><kbd>Cmd</kbd>+<kbd>O</kbd></td><td>Opens an audio file (WAV, MP3, FLAC, AIFF, OGG, M4A) or a PitchNet project (<code>.pitchnet</code>). See <a href="projects.html">Opening, saving and exporting</a>.</td></tr>
<tr><td>Recent Files</td><td></td><td>The files you opened most recently. Shows <i>No Recent Files</i> when the list is empty.</td></tr>
<tr><td>Save Project</td><td><kbd>Cmd</kbd>+<kbd>S</kbd></td><td>Saves the edits, analysis and settings to a <code>.pitchnet</code> project.</td></tr>
<tr><td>Save Project As&hellip;</td><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>S</kbd></td><td>Saves the project to a new file.</td></tr>
<tr><td>Export Audio&hellip;</td><td><kbd>Cmd</kbd>+<kbd>E</kbd></td><td>Renders the edited audio to a file. See <a href="projects.html#export">Export Settings</a>.</td></tr>
<tr><td>Export MIDI&hellip;</td><td></td><td>Writes the notes as a Standard MIDI File.</td></tr>
<tr><td>Settings / About / Quit</td><td></td><td>On Windows and Linux these are in the File menu.</td></tr>
</table>

<h2 id="edit">Edit menu</h2>
<table><tr><th>Command</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>Undo</td><td><kbd>Cmd</kbd>+<kbd>Z</kbd></td><td>Undoes the last edit. Every edit can be undone.</td></tr>
<tr><td>Redo</td><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>Z</kbd> or <kbd>Cmd</kbd>+<kbd>Y</kbd></td><td>Redoes the last undone edit.</td></tr>
<tr><td>Cut / Copy / Paste</td><td><kbd>Cmd</kbd>+<kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd></td><td>For <a href="polyphonic.html">polyphonic</a> takes: cuts or copies the selected notes, and pastes them at the playhead.</td></tr>
<tr><td>Select All</td><td><kbd>Cmd</kbd>+<kbd>A</kbd></td><td>Selects every note.</td></tr>
<tr><td>Fourier Pitch Filter&hellip;</td><td></td><td>Smooths or isolates parts of the pitch curve. See <a href="correct-pitch.html#fourier">Fourier Pitch Filter</a>.</td></tr>
</table>

<h2 id="view">View commands</h2>
<p>These commands have shortcuts but no menu item:</p>
<table><tr><th>Command</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>Show Delta Pitch</td><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>D</kbd></td><td>Shows or hides the delta pitch curve (the pitch detail on top of the note's base pitch).</td></tr>
<tr><td>Show Base Pitch</td><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>B</kbd></td><td>Shows or hides the base pitch curve (the smooth line the notes form).</td></tr>
</table>
""")

# ---------------------------------------------------------------- Toolbar
page("toolbar", "Toolbar", f"""
<p>The toolbar runs along the top of the window. From left to right it holds the transport controls, the edit tools,
the selection fields, the scale selector and the action buttons.</p>
[[fig:toolbar.png|The PitchNet toolbar.]]

<h2 id="transport">Transport</h2>
<table><tr><th></th><th>Button</th><th>Shortcut</th><th>What it does</th></tr>
<tr><td>{ico('stop')}</td><td>Stop</td><td><kbd>Esc</kbd></td><td>Stops playback.</td></tr>
<tr><td>{ico('play')}</td><td>Play / Pause</td><td><kbd>Space</kbd></td><td>Plays from the playhead, or pauses. The button shows Pause while playing.</td></tr>
<tr><td>{ico('cycle')}</td><td>Loop</td><td></td><td>Repeats the range set in the <a href="piano-roll.html#loop">loop bar</a>.</td></tr>
<tr><td>{ico('scroll')}</td><td>Follow (Auto Scroll)</td><td></td><td>Keeps the playhead in view while playing.</td></tr>
<tr><td>{ico('record')}</td><td>Capture</td><td></td><td>Plug-in only, in hosts without ARA: records the track's audio into PitchNet. See <a href="plugin.html#capture">Capture</a>.</td></tr>
</table>
<p><kbd>Home</kbd> moves the playhead to the start and <kbd>End</kbd> to the end. Clicking in the
<a href="piano-roll.html#timeline">timeline</a> moves the playhead.</p>

<h2 id="tools">Edit tools</h2>
<table><tr><th></th><th>Tool</th><th>Shortcut</th></tr>
<tr><td>{ico('select')}</td><td><a href="edit-tools.html#main">Main Tool</a></td><td><kbd>1</kbd></td></tr>
<tr><td>{ico('split')}</td><td><a href="edit-tools.html#split">Note Separation Tool</a></td><td><kbd>2</kbd></td></tr>
<tr><td>{ico('drawing')}</td><td><a href="edit-tools.html#drawing">Pitch Drawing Tool</a></td><td><kbd>3</kbd></td></tr>
<tr><td>{ico('timing')}</td><td><a href="edit-tools.html#timing">Timing Tool</a></td><td><kbd>4</kbd></td></tr>
</table>

<h2 id="selection">Selection fields</h2>
<p>When notes are selected, the toolbar shows three fields for them:</p>
<ul>
<li><b>Note</b> &ndash; the pitch of the selected notes. Drag it, type a note name (<code>E4</code>), or type <code>+2</code> / <code>-1</code> to transpose them all by semitones.</li>
<li><b>Cent</b> &ndash; cents from the nearest semitone. Drag or type a value; every selected note moves by the same amount.</li>
<li><b>Level</b> &ndash; the level of the selected notes in dB. Drag it, or double-click for 0&nbsp;dB.</li>
</ul>
<p>For <a href="polyphonic.html">polyphonic</a> takes these fields replace the edit tools, next to a <b>Note Assignment Mode</b> button.
In Note Assignment Mode the toolbar shows a two-handle slider: <b>Note threshold</b> (how likely a sound must be to count as a note) and <b>Candidates</b>
(which other possible notes are shown). Double-click either handle to reset it.</p>

<h2 id="scale">Scale selector</h2>
<p>Shows the current key and scale (for example <b>C Major</b>). Click it to choose a root and mode. The scale is used by
<b>Drag Snap</b>, <b>Double Click Snap</b> and <b>Correct Pitch</b>, and its notes are highlighted in the piano roll.
See <a href="correct-pitch.html#scale">Scales</a>.</p>

<h2 id="actions">Action buttons</h2>
<table><tr><th></th><th>Button</th><th>What it does</th></tr>
<tr id="macro"><td>{ico('quantize')}</td><td>Correct Pitch Macro</td><td>Opens <a href="correct-pitch.html#macro">Correct Pitch</a> for the selected notes (or all notes).</td></tr>
<tr id="audition"><td>{ico('audition')}</td><td>Live Audition On/Off</td><td>When on, you hear a note while you drag it.</td></tr>
<tr id="undo"><td>{ico('undo')} {ico('redo')}</td><td>Undo / Redo</td><td><kbd>Cmd</kbd>+<kbd>Z</kbd> / <kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>Z</kbd>.</td></tr>
<tr id="help"><td><span class="ico"><img src="images/icons/btn-help.png" alt=""></span></td><td>Online Manual</td><td>Opens this manual in your web browser.</td></tr>
<tr><td>{ico('side')}</td><td>Parameters</td><td>Shows or hides the <a href="parameters.html">Parameters panel</a> on the right.</td></tr>
<tr><td>{ico('tracks')}</td><td>Tracks</td><td>Shows or hides the <a href="tracks.html">Tracks panel</a> on the left (when the project has tracks).</td></tr>
<tr><td>{ico('region')}</td><td>Clip / Track mode</td><td>ARA plug-in only. <b>Clip mode</b> shows the selected clip's source; <b>Track mode</b> shows every clip on the track.</td></tr>
</table>
""")

# ---------------------------------------------------------------- Edit tools
page("edit-tools", "Edit Tools", f"""
<p>PitchNet has four edit tools. Choose one in the toolbar or press its number key. You can also right-click the
canvas to switch tools from the menu.</p>

<h2 id="main">{ico('select')} Main Tool <kbd>1</kbd></h2>
<p>The tool you will use most. It selects notes and changes their pitch and level.</p>
<ul>
<li><b>Click</b> a note to select it; <b>drag</b> a box in an empty area to select several. <kbd>Cmd</kbd>-click adds a note to, or removes it from, the selection; <kbd>Cmd</kbd>-drag adds a box to it.</li>
<li><b>Drag</b> a note up or down to change its pitch. Dragging a selected note moves every selected note.
With <a href="parameters.html#pitch">Drag Snap</a> on, notes move in semitones (or scale steps); hold <kbd>Option</kbd> to move freely.</li>
<li><b>Double-click</b> a note to snap it to its pitch centre (see <a href="parameters.html#pitch">Double Click Snap</a>).</li>
<li>Selecting a single note shows its <a href="note-handles.html">note handles</a> for drift, modulation, slopes, level, formant and sibilant balance.</li>
<li>Hovering over a note shows two small buttons: {ico('preview')} <b>Context Audition</b> plays the note with a little audio around it, and
{ico('reset')} <b>Restore Original</b> opens the <a href="correct-pitch.html#restore">restore menu</a>.</li>
<li>Drag a <a href="breaths-sibilance.html#breath-notes">breath note</a> up or down to change its level; double-click it to reset.</li>
</ul>

<h2 id="split">{ico('split')} Note Separation Tool <kbd>2</kbd></h2>
<p>Splits and joins notes.</p>
<ul>
<li><b>Click</b> inside a note to split it in two at that point.</li>
<li><b>Click</b> on the boundary where two notes meet to merge them into one note.</li>
</ul>
<p>Use it when the analysis joined two sung syllables into one note, or split one long note in two.</p>

<h2 id="drawing">{ico('drawing')} Pitch Drawing Tool <kbd>3</kbd></h2>
<p>Draws a new pitch curve over the existing one. See <a href="pitch-drawing.html">Drawing pitch curves</a> for the full workflow.</p>
<ul>
<li><b>Click</b> to place anchor points; the new curve runs smoothly between them.</li>
<li>A panel asks <i>Apply new pitch curve?</i> Click the tick to apply or the cross to cancel. Cancelling puts the curve back exactly as it was.</li>
</ul>

<h2 id="timing">{ico('timing')} Timing Tool <kbd>4</kbd></h2>
<p>Moves and stretches notes in time. See <a href="timing.html">Timing edits</a>.</p>
<ul>
<li>Drag the start or end of a note, or the boundary between two notes.</li>
<li>Select several notes to move their boundaries together.</li>
<li>Stretching holds the vowel: consonants keep their recorded length and move with the note.</li>
</ul>
""")

# ---------------------------------------------------------------- Piano roll
page("piano-roll", "Piano Roll", f"""
<p>The piano roll is the main editing area. Time runs from left to right and pitch from bottom to top.</p>
[[fig:main-window.jpg|The piano roll with an analysed vocal (a phrase from the vocadito dataset).]]

<h2 id="timeline">Timeline</h2>
<p>The ruler at the top shows time in minutes and seconds, or in bars and beats when the timeline is in
<b>Beats</b> mode (<a href="parameters.html#time">Parameters &rsaquo; Time</a>).</p>
<ul>
<li>Click in the timeline to move the playhead.</li>
<li>Scroll over the timeline to zoom in or out horizontally.</li>
<li>Click and drag with the middle mouse button anywhere in the timeline or canvas to scrub.</li>
</ul>

<h2 id="loop">Loop bar</h2>
<p>The strip below the timeline sets the loop range. Drag in it to set the range, and drag its ends to adjust it.
Turn on {ico('cycle')} <b>Loop</b> in the toolbar to repeat the range during playback.</p>

<h2 id="keys">Piano keys</h2>
<p>The note names down the left side. Rows of notes that are in the current <a href="correct-pitch.html#scale">scale</a>
are shaded lighter. Scroll over the keys to zoom vertically.</p>

<h2 id="canvas">Note canvas</h2>
<p>Each <b>note</b> is drawn as a blob. Its height follows the loudness of the audio, its colour helps tell neighbouring
notes apart, and its vertical position is the note's pitch. The faint waveform behind the notes shows the whole recording.</p>
<ul>
<li>The <b>pitch curve</b> (white line) is the detected pitch, after your edits. It is left out over breaths and sibilants, which play the original recording.</li>
<li><b>Hatched</b> parts of a note are breaths or sibilants (&ldquo;s&rdquo;, &ldquo;sh&rdquo;, &ldquo;t&rdquo;&hellip;). See <a href="breaths-sibilance.html">Breaths and sibilants</a>.</li>
<li>When a <a href="midi.html#reference">MIDI reference</a> is loaded, its notes are drawn as outlines behind the blobs.</li>
<li>The <a href="chord-track.html">Chord Track</a> can be shown above the notes.</li>
</ul>

<h2 id="view-controls">View controls</h2>
<p>The floating controls at the bottom right of the piano roll change the view:</p>
<table><tr><th></th><th>Control</th><th>What it does</th></tr>
<tr><td></td><td>Blob Height</td><td>The short vertical slider. Makes note blobs taller or flatter. Double-click to reset.</td></tr>
<tr><td></td><td>Vertical Zoom</td><td>The tall vertical slider. Changes how many semitones fit on screen.</td></tr>
<tr><td>{ico('autozoom')}</td><td>Vertical Auto-Zoom</td><td>Zooms and scrolls so all notes fit vertically.</td></tr>
<tr><td></td><td>Horizontal Zoom</td><td>The horizontal slider. Changes how much time fits on screen.</td></tr>
<tr><td>{ico('thumbnail')}</td><td>Overview</td><td>Shows or hides the overview strip.</td></tr>
</table>

<h3>Scrolling and zooming</h3>
<table><tr><th>Do this</th><th>To</th></tr>
<tr><td>Scroll wheel / two-finger swipe</td><td>Scroll up, down, left and right</td></tr>
<tr><td><kbd>Shift</kbd> + scroll</td><td>Scroll sideways with a vertical wheel</td></tr>
<tr><td><kbd>Cmd</kbd> + scroll</td><td>Zoom in both directions around the pointer</td></tr>
<tr><td>Scroll over the piano keys</td><td>Zoom vertically</td></tr>
<tr><td>Scroll over the timeline</td><td>Zoom horizontally</td></tr>
<tr><td><kbd>Option</kbd> + drag in an empty area</td><td>Pan the view</td></tr>
</table>

<h2 id="overview">Overview</h2>
<p>The overview at the bottom of the window shows the whole recording in miniature, with the visible part outlined.
Drag the outline to move through the recording, or drag its edges to zoom.</p>

<h2 id="context-menu">Right-click menu</h2>
<p>Right-clicking the canvas opens a menu to switch tools (Select, Split, Drawing, Timing) and to <b>Select All</b>.
Right-clicking a note selects it (if it was not selected) and offers <a href="correct-pitch.html#restore">Restore Original</a>.
For polyphonic takes the menu also has <b>Cut</b>, <b>Copy</b>, <b>Paste</b>, <b>Delete</b>, <b>Deactivate Notes</b> and <b>Resume</b>.</p>
""")

# ---------------------------------------------------------------- Note handles
page("note-handles", "Note Handles", f"""
<p>When exactly one note is selected with the <a href="edit-tools.html#main">Main Tool</a>, small handles appear
around it. Each handle changes one aspect of the note. Drag a handle up or down; double-click it to reset it.
Hover over a handle to see its name and current value.</p>
[[fig:note-handles.jpg|A selected note. Above it: Pitch Drift, Pitch Modulation and Fine Pitch; below it: Sibilant Balance, Amplitude and Formant Shift, then Context Audition and Restore Original.]]
<p>The three handles <b>above</b> the note are, from left to right, <b>Pitch Drift</b>, <b>Pitch Modulation</b> and <b>Fine Pitch</b>.
The three <b>below</b> it are <b>Sibilant Balance</b>, <b>Amplitude</b> and <b>Formant Shift</b>. Under them are the
{ico('preview')} Context Audition and {ico('reset')} Restore Original buttons.</p>

<table>
<tr><th></th><th>Handle</th><th>What it changes</th><th>Double-click</th></tr>
<tr><td><img src="images/icons/cent.png" alt="" width="16"></td><td><b>Fine Pitch</b> (cents)</td><td>Moves the whole note by cents, like dragging the note itself.</td><td>Puts the note back on its nearest semitone.</td></tr>
<tr><td><img src="images/icons/drift.png" alt="" width="16"></td><td><b>Pitch Drift</b></td><td>Scales the slow wandering of pitch within the note. Down straightens the note; up exaggerates the drift.</td><td>Resets.</td></tr>
<tr><td><img src="images/icons/vibrato.png" alt="" width="16"></td><td><b>Pitch Modulation</b></td><td>Scales the fast movement in the pitch curve &ndash; mostly vibrato. Down flattens vibrato; up deepens it.</td><td>Resets.</td></tr>
<tr><td><img src="images/icons/ltilt.png" alt="" width="16"> <img src="images/icons/rtilt.png" alt="" width="16"></td><td><b>Left Slope</b> / <b>Right Slope</b></td><td>Tilts the pitch at the start or end of the note &ndash; useful for scoops into a note and falls at its end. Hold <kbd>Option</kbd> (<kbd>Alt</kbd>) over the pitch handles to swap them for the slope handles.</td><td>Resets.</td></tr>
<tr><td><img src="images/icons/amplitude.png" alt="" width="16"></td><td><b>Amplitude</b></td><td>The note's level in dB.</td><td>0&nbsp;dB.</td></tr>
<tr><td><img src="images/icons/formant.png" alt="" width="16"></td><td><b>Formant Shift</b></td><td>Moves the formants (the tone colour of the voice) up or down without changing the pitch. Higher sounds smaller and brighter; lower sounds bigger and darker.</td><td>Resets.</td></tr>
<tr><td><img src="images/icons/sibilant.png" alt="" width="16"></td><td><b>Sibilant Balance</b></td><td>Turns down the note's sibilants (hatched frames), or the rest of the note. See <a href="breaths-sibilance.html#sibilant-balance">Sibilant Balance</a>.</td><td>Mutes the sibilants (&minus;100&nbsp;%); again restores them (0&nbsp;%).</td></tr>
</table>

<div class="tip"><b>Tip:</b> To change these values for several notes at once, select them and use
<a href="correct-pitch.html#macro">Correct Pitch</a> (pitch centre and drift), the <b>Level</b> field in the
<a href="toolbar.html#selection">toolbar</a>, or the global <a href="parameters.html#rendering">Formant</a> control.</div>
""")

# ---------------------------------------------------------------- Pitch drawing
page("pitch-drawing", "Drawing Pitch Curves", f"""
<p>The {ico('drawing')} <b>Pitch Drawing Tool</b> (<kbd>3</kbd>) replaces part of a pitch curve with one you draw.
Use it for things the note handles cannot do: a custom scoop, a specific fall-off, or repairing a glitch in the detected pitch.</p>

<h2>Drawing a curve</h2>
<ol>
<li>Press <kbd>3</kbd>.</li>
<li>Click where the new curve should start. Each further click adds an anchor point. The new line is drawn between the points with a smooth
(&ldquo;smootherstep&rdquo;) transition, flat at each anchor.</li>
<li>Drag an anchor to move it. The preview updates as you drag, and you hear the result.</li>
<li>Click the tick in the <i>Apply new pitch curve?</i> panel to keep it, or the cross to discard it.</li>
</ol>
<p>While points are being placed, the line is dashed over breaths and sibilants: those frames keep the original recording.</p>

<div class="note"><b>Note:</b> Applying a drawing is one undo step. Cancelling restores the previous curves exactly, including the
pitch of frames between notes.</div>

<h2 id="fourier">Fourier Pitch Filter</h2>
<p><b>Edit &rsaquo; Fourier Pitch Filter&hellip;</b> filters the pitch curve of the selected notes in the frequency domain.
Set <b>Highpass (Hz)</b> to remove slow pitch movement (drift), and <b>Lowpass (Hz)</b> to remove fast movement (jitter and vibrato).
For example, a lowpass of a few Hz smooths a shaky note while keeping its overall shape.</p>
""")

# ---------------------------------------------------------------- Timing
page("timing", "Timing Edits", f"""
<p>The {ico('timing')} <b>Timing Tool</b> (<kbd>4</kbd>) changes when notes start and end.</p>

<h2>Moving and stretching</h2>
<ul>
<li><b>Drag a note's start or end</b> to make it longer or shorter.</li>
<li><b>Drag the boundary between two notes</b> to move it: one note gets longer and the other shorter.</li>
<li><b>Select notes</b> first to move all their boundaries together; their distances are kept.</li>
<li>A boundary next to a rest can approach the next note, but never cross it.</li>
</ul>

<h2 id="vowel">Consonants keep their length</h2>
<p>When a note is stretched or squeezed, PitchNet holds the vowel: the consonant at the start of the note keeps its
recorded length and moves with the note, and the vowel takes up the change. Breaths and sibilants inside the note
play the original recording, copied from where they were recorded. This keeps words intelligible even after large timing changes.</p>

<h2 id="snap">Snapping</h2>
<p>With <b>Drag Snap</b> on in <a href="parameters.html#time">Parameters &rsaquo; Time</a> (Beats mode), the boundary you grab snaps to the grid.
When you move a note's start, it is the <i>vowel onset</i> that lands on the grid, not the consonant &ndash; so the
note sounds on the beat. Hold <kbd>Option</kbd> to invert snapping while dragging.</p>
""")

# ---------------------------------------------------------------- Correct pitch / scale / restore
page("correct-pitch", "Correct Pitch, Scales and Restore", f"""
<h2 id="macro">{ico('quantize')} Correct Pitch Macro</h2>
<p>Corrects many notes at once. Select the notes (or none for all notes) and click <b>Correct Pitch Macro</b> in the toolbar.</p>
[[fig:correct-pitch.jpg|The Correct Pitch panel opens under its toolbar button.]]
<table><tr><th>Setting</th><th>What it does</th></tr>
<tr><td><b>Pitch Center</b> (0&ndash;100&nbsp;%)</td><td>How far each note's average pitch moves toward its target note. 100&nbsp;% puts it exactly on the note;
lower values keep some of the original intonation.</td></tr>
<tr><td><b>Pitch Drift</b> (0&ndash;100&nbsp;%)</td><td>How much of the slow pitch wandering inside each note is removed. Vibrato is kept.</td></tr>
<tr><td><b>Snap to Scale</b></td><td>When on, the targets are the notes of the current scale; when off, the nearest semitone.</td></tr>
</table>
<p>The macro is one undo step, and each note can still be edited or restored afterwards.</p>

<h2 id="scale">Scales</h2>
<p>Click the scale selector in the toolbar (it shows, for example, <b>C Major</b>) to choose a <b>Root</b> and a <b>Mode</b>:</p>
<p>Major, Minor, Harmonic Minor, Melodic Minor, Dorian, Phrygian, Phrygian Dominant, Lydian, Mixolydian, Locrian,
Major Pentatonic, Minor Pentatonic, Blues and Whole Tone.</p>
<p>In the plug-in with ARA, <b>Use Project Key</b> follows the key set in the host.</p>
<p>The scale is used by:</p>
<ul>
<li><b>Drag Snap</b> set to <i>Scale</i> &ndash; notes move in scale steps.</li>
<li><b>Double Click Snap</b> set to <i>Scale</i> &ndash; double-clicking a note moves it to the nearest scale note.</li>
<li><b>Correct Pitch</b> with <b>Snap to Scale</b>.</li>
</ul>
<p>Set Drag Snap and Double Click Snap in <a href="parameters.html#pitch">Parameters &rsaquo; Pitch</a>.</p>

<h2 id="restore">{ico('reset')} Restore Original</h2>
<p>Hover over a note and click the Restore Original button, or right-click the note. The menu restores one kind of edit
for the note (or for all selected notes):</p>
<ul>
<li><b>Pitch</b> &ndash; pitch, drift, modulation, slopes and drawn curves.</li>
<li><b>Formant</b> &ndash; the formant shift.</li>
<li><b>Timing</b> &ndash; the note's position and length.</li>
<li><b>Amplitude</b> &ndash; the level and sibilant balance.</li>
<li><b>All Edits</b> &ndash; everything above.</li>
</ul>

<h2 id="fourier">Fourier Pitch Filter</h2>
<p>See <a href="pitch-drawing.html#fourier">Drawing pitch curves &rsaquo; Fourier Pitch Filter</a>.</p>
""")

# ---------------------------------------------------------------- Breaths
page("breaths-sibilance", "Breaths and Sibilants", """
<p>PitchNet labels every frame of a vocal as <b>sibilance</b>, <b>breath</b> or neither, and treats each kind differently
so that edits sound natural.</p>

<h2>What you see</h2>
<ul>
<li>Inside a note, sibilant and breath frames are <b>hatched</b>, and the pitch curve is left out over them.</li>
<li>Every breath between notes is drawn as its own <b>breath note</b>.</li>
<li>Turn the display on or off with <b>Show Breaths &amp; Sibilance</b> in <a href="parameters.html#ui">Parameters &rsaquo; UI</a>.</li>
</ul>

<h2>What happens to them</h2>
<ul>
<li><b>Consonants join the note they lead into.</b> The &ldquo;s&rdquo; in &ldquo;sun&rdquo; belongs to the note &ldquo;sun&rdquo;, so moving the note moves the consonant too.
A word-final consonant before a pause stays with its note.</li>
<li><b>Breaths stay out of notes.</b></li>
<li><b>Breaths and sibilants keep the original recording</b> when notes are retuned. They carry no pitch to correct, so they are not resynthesised &ndash; they come back bit-identical.</li>
<li><b>Stretching a note holds its vowel</b> &ndash; see <a href="timing.html#vowel">Timing edits</a>.</li>
</ul>

<h2 id="breath-notes">Breath notes</h2>
<p>Drag a breath note up or down with the Main Tool to make the breath louder or quieter. Double-click it to reset its level.
The waveform behind it follows the level while you drag.</p>

<h2 id="sibilant-balance">Sibilant Balance</h2>
<p>Each note has a <b>Sibilant Balance</b> <a href="note-handles.html">handle</a>, similar to the one in Melodyne.</p>
<ul>
<li>Drag it down to turn down the note's sibilants (a per-note de-esser).</li>
<li>Drag it up to turn down the rest of the note, leaving the sibilants.</li>
<li>Double-click it to mute the sibilants (&minus;100&nbsp;%); double-click again to restore the note (0&nbsp;%).</li>
</ul>
<p>Stray sibilance just outside a note (such as a final &ldquo;s&rdquo; the note stops short of) belongs to that note, so turning a note or its sibilants down leaves no stray &ldquo;s&rdquo; behind.</p>
""")

# ---------------------------------------------------------------- Analysis
page("analysis", "How Analysis Works", """
<p>When you open audio, PitchNet analyses it in several stages. Knowing them helps when a result is not what you expected.</p>

<h2>Stages</h2>
<ol>
<li><b>Input type.</b> A classifier decides whether the recording is a solo voice or several instruments (polyphonic).
You can override it with <b>Input</b> in <a href="parameters.html#analysis">Parameters &rsaquo; Rendering</a>: <i>Automatic</i>, <i>Vocal</i> or <i>Polyphonic</i>.</li>
<li><b>Pitch detection.</b> A neural pitch detector (SwiftF0 by default; FCPE and RMVPE are also included) estimates the pitch (F0) of every 512-sample frame.</li>
<li><b>Voicing and breath/sibilance labels.</b> Each frame is marked voiced or unvoiced, and unvoiced frames are labelled breath or sibilance.</li>
<li><b>Note segmentation.</b> The GAME model splits the pitch into notes; PitchNet then moves note edges so that consonants join the note they lead into.</li>
<li><b>Mel spectrogram.</b> Prepared for the neural vocoder, so that edits can be previewed instantly.</li>
</ol>
<p>For polyphonic input, analysis instead runs the <a href="polyphonic.html">polyphonic engine</a>.</p>

<h2 id="reanalyze">Analysing again</h2>
<p>Changing the <b>Input</b> type, or choosing <b>Re-analyze</b> when PitchNet offers it, analyses the audio again.
This discards your edits to that take and cannot be undone, so PitchNet asks first.</p>

<p>If pitch extraction fails on the GPU, PitchNet retries on the CPU. If it still fails, see <a href="troubleshooting.html">Troubleshooting</a>.</p>
""")

# ---------------------------------------------------------------- Polyphonic
page("polyphonic", "Polyphonic Editing", f"""
<p>PitchNet can edit recordings where several notes sound at once &ndash; a piano chord, a guitar strum, a string section.
Each note can be re-pitched, re-gained, muted, moved in time, lengthened or shortened on its own, while everything you do not edit
comes back untouched.</p>

[[fig:polyphonic.jpg|A piano chord progression (C &ndash; Am &ndash; F &ndash; G) analysed as a polyphonic take. Every note of every chord is its own note, the chords are named in the Chord Track, and the timeline shows bars from the detected tempo.]]

<h2>Opening polyphonic audio</h2>
<p>Open the file as usual. If the input classifier hears instruments rather than a voice, the take is analysed as polyphonic.
To force it, set <b>Input</b> to <i>Polyphonic</i> in <a href="parameters.html#analysis">Parameters &rsaquo; Rendering</a>.</p>
<p>After analysis:</p>
<ul>
<li>Each note is drawn as a thin blob on its pitch; its thickness follows the note's level, so you can see each note decay.</li>
<li>The <a href="chord-track.html">Chord Track</a> names the chords the notes play.</li>
<li>If the project still has the default tempo, PitchNet <b>detects the tempo and the first downbeat</b> from the notes and switches the timeline to bars and beats,
so timing edits can snap to the music's own grid. In the plug-in, the host's tempo is used instead.</li>
<li>The toolbar shows the <b>Note</b>, <b>Cent</b> and <b>Level</b> fields for the selected notes and the
<b>Note Assignment Mode</b> button in place of the vocal edit tools.</li>
</ul>

<h2>Detectors</h2>
<table><tr><th>Detector</th><th>How it works</th></tr>
<tr><td><b>Harmonic</b></td><td>Finds notes from the harmonic patterns in the sound; a neural note detector confirms them. The default.</td></tr>
<tr><td><b>Guided</b></td><td>A neural note detector proposes the notes and the harmonic analysis refines them. Try it when Harmonic misses notes.</td></tr>
</table>

<h2>Editing notes</h2>
<ul>
<li>Drag notes to re-pitch them, and use the <b>Level</b> field to change their level.</li>
<li>Use the Timing Tool to move or stretch them. A lengthened decaying note (piano, harp) rings on naturally instead of slowing down.</li>
<li><b>Cut</b>, <b>Copy</b> and <b>Paste</b> (<kbd>Cmd</kbd>+<kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd>) work on notes: paste places the copied notes at the playhead.</li>
<li><b>Delete</b> and <b>Deactivate Notes</b> (right-click menu) remove notes from the sound; <b>Resume</b> brings deactivated notes back.</li>
</ul>

<h2 id="assignment">Note Assignment Mode</h2>
<p>Sometimes the analysis misses a note or finds one that is not there. Turn on <b>Note Assignment Mode</b> in the toolbar
to see the other <i>candidate</i> notes PitchNet considered. Candidates are drawn as dashed outlines; click one to make it a note.</p>
[[fig:polyphonic-assignment.jpg|Note Assignment Mode: the toolbar shows the threshold slider, and candidate notes appear as dashed outlines (for example the F4 in bar 3 and the A2 in bar 2).]]
<ul>
<li><b>Note threshold</b> (left handle of the slider): how likely a sound must be to be a note. Drag left for fewer notes, right for more.</li>
<li><b>Candidates</b> (right handle): which other possible notes are shown. Drag right to show more.</li>
<li>Double-click a handle to reset it.</li>
</ul>

<div class="note"><b>Good to know:</b> Only pitched sound is modelled. Attack noise, breath and percussion are not notes of their own;
by default each edited note takes its share of that unpitched sound with it.</div>
""")

# ---------------------------------------------------------------- Rendering
page("rendering", "Rendering Engines", """
<p>Every edit is rendered in the background as you work. Choose how in <a href="parameters.html#rendering">Parameters &rsaquo; Rendering</a>.</p>

<table><tr><th>Engine</th><th>How it works</th><th>Best for</th></tr>
<tr><td><b>AI Resynthesis</b></td><td>Neural vocoder (PC-NSF-HiFiGAN). Rebuilds the voice from its spectrum, and reshapes tone across the whole edited region.</td>
<td>Large pitch moves and formant changes. Available on macOS, and on Windows with DirectML.</td></tr>
<tr><td><b>Classic</b></td><td>Classic DSP (time-domain PSOLA). Shifts the original recording itself. Untouched audio stays identical and no model is needed.</td>
<td>Small corrections; much faster on the CPU. Large pitch moves sound rougher.</td></tr>
</table>

<h2>Device</h2>
<p>The <b>Device</b> menu in the Rendering section chooses where neural inference runs: the CPU, or a GPU / Neural Engine
(CoreML on macOS, DirectML or CUDA on Windows). It is shown when there is more than one choice.
You cannot switch device while inference is running.</p>

<h2>Global controls</h2>
<ul>
<li><b>Global Pitch (st)</b> transposes the whole take.</li>
<li><b>Formant Shift (st)</b> shifts the formants of the whole take.</li>
<li><b>Volume (dB)</b> changes the output level.</li>
</ul>
""")

# ---------------------------------------------------------------- Chord track
page("chord-track", "Chord Track", """
<p>The Chord Track shows the chords that the notes play, above the piano roll. Turn it on with
<b>Show Chord Track</b> in <a href="parameters.html#ui">Parameters &rsaquo; UI</a>. In the plug-in, it appears only when the host provides chords.</p>
<ul>
<li><b>Click a chord's name</b> to change it: type a chord such as <code>Cm7/G</code>. The notes follow the new chord.
If PitchNet cannot read the text it says so; if the notes already fit the chord, or no note could become a missing chord tone, it tells you.</li>
<li><b>Click elsewhere on a chord</b> to split it.</li>
<li><b>Click where two chords meet</b> to merge them.</li>
<li><b>Drag a chord edge</b> to resize it.</li>
<li><b>Detect Chords Again</b> rebuilds the chords from the notes.</li>
</ul>
""")

# ---------------------------------------------------------------- MIDI
page("midi", "MIDI Reference and Export", """
<h2 id="export">Exporting notes as MIDI</h2>
<ul>
<li><b>File &rsaquo; Export MIDI&hellip;</b> writes the notes to a Standard MIDI File.</li>
<li><b>Drag to DAW</b> in <a href="parameters.html#midi">Parameters &rsaquo; MIDI</a>: drag it onto a DAW track to drop the notes as MIDI,
or click it to save a <code>.mid</code> file.</li>
</ul>
<p>If there are no notes yet, PitchNet asks you to load and analyse audio first.</p>

<h2 id="reference">MIDI reference</h2>
<p>A MIDI reference is a guide melody drawn behind the notes &ndash; for example the melody from the song's score or a guide vocal's MIDI.</p>
<ol>
<li>In <a href="parameters.html#midi">Parameters &rsaquo; MIDI</a>, click <b>Import Reference</b> and choose a <code>.mid</code> file.</li>
<li>Choose the <b>Track</b> to show if the file has several.</li>
<li>Turn <b>Show Reference</b> on or off.</li>
</ol>
<p>With <b>Double Click Snap</b> set to <b>MIDI Ref.</b>, double-clicking a note moves it to the reference note under it.</p>
""")

# ---------------------------------------------------------------- Parameters
page("parameters", "Parameters Panel", f"""
<p>Click {ico('side')} <b>Parameters</b> at the right end of the toolbar to show or hide the Parameters panel.
It holds the settings of the current project in cards, from top to bottom: Rendering, Language, Time, Pitch, MIDI and UI.
Scroll the panel to reach the lower cards.</p>
[[fig:parameters.png|The Parameters panel (top and bottom).]]

<h2 id="rendering">Rendering</h2>
<table><tr><th>Control</th><th>What it does</th></tr>
<tr><td><b>AI Resynthesis</b> / <b>Classic</b></td><td>The rendering engine. See <a href="rendering.html">Rendering engines</a>.</td></tr>
<tr><td><b>Device</b></td><td>Where neural inference runs (CPU, or GPU / Neural Engine). Shown when there is more than one choice.</td></tr>
<tr id="analysis"><td><b>Input</b></td><td><i>Automatic</i>, <i>Vocal</i> or <i>Polyphonic</i>: how the take is analysed. Shows <i>Not analysed</i> before analysis.
Changing it analyses the take again and discards its edits, so PitchNet asks first.</td></tr>
<tr><td><b>Harmonic</b> / <b>Guided</b></td><td>For <a href="polyphonic.html">polyphonic</a> takes: the note detector.</td></tr>
</table>

<h2 id="language">Language</h2>
<p>The language of PitchNet's interface; <i>System default</i> follows your system. See <a href="languages.html">Languages</a>.</p>

<h2 id="time">Time</h2>
<table><tr><th>Control</th><th>What it does</th></tr>
<tr><td><b>Beats</b> / <b>Time</b></td><td>Shows the timeline in bars and beats, or in minutes and seconds.</td></tr>
<tr><td><b>Beat</b></td><td>Time signature, for example 4/4.</td></tr>
<tr><td><b>Tempo</b></td><td>Tempo in BPM. In the plug-in it follows the host.</td></tr>
<tr><td><b>Grid</b></td><td>The grid resolution for timing snaps, for example 1/4 or 1/16.</td></tr>
<tr><td><b>Drag Snap</b></td><td>Snaps timing edits to the grid. Time mode has no grid, so this works in Beats mode only.</td></tr>
</table>

<h2 id="pitch">Pitch</h2>
<table><tr><th>Control</th><th>What it does</th></tr>
<tr><td><b>Chromatic</b> / <b>Scale</b></td><td>Whether the piano roll and snapping use all 12 semitones or the current <a href="correct-pitch.html#scale">scale</a>.</td></tr>
<tr><td><b>Reference (A4)</b></td><td>Tuning reference, 440&nbsp;Hz by default. Change it for music tuned to, for example, 432 or 442&nbsp;Hz.</td></tr>
<tr><td><b>Drag Snap</b></td><td>When on, dragging notes moves them in whole steps &ndash; <i>Chromatic</i> (semitones) or <i>Scale</i> (scale notes), chosen with the button beside it. Hold <kbd>Option</kbd> while dragging to invert it.</td></tr>
<tr><td><b>Double Click Snap</b></td><td>Where a double-clicked note goes: <i>Pitch Center</i> (nearest semitone), <i>Scale</i> (nearest scale note) or <i>MIDI Ref.</i> (the <a href="midi.html#reference">MIDI reference</a> note).</td></tr>
</table>

<h2 id="midi">MIDI</h2>
<ul>
<li><b>Drag to DAW</b> &ndash; drag the notes onto a DAW track as MIDI, or click to save a <code>.mid</code> file.</li>
<li><b>Import Reference</b>, the track menu beside it, and <b>Show Reference</b> &ndash; see <a href="midi.html#reference">MIDI reference</a>.</li>
</ul>

<h2 id="ui">UI</h2>
<ul>
<li><b>Show Breaths &amp; Sibilance</b> &ndash; hatching and breath notes. See <a href="breaths-sibilance.html">Breaths and sibilants</a>.</li>
<li><b>Show Chord Track</b> &ndash; see <a href="chord-track.html">Chord Track</a>.</li>
<li><b>Brightness</b> &ndash; the brightness of the piano roll display.</li>
</ul>
""")

# ---------------------------------------------------------------- Tracks
page("tracks", "Tracks Panel", f"""
<p>The Tracks panel lists the tracks in the session. It is mainly used in the <a href="plugin.html">plug-in with ARA</a>,
where PitchNet sees every track and clip you added it to. Show it with {ico('tracks')} <b>Tracks</b> in the toolbar.</p>
<ul>
<li>Click a track to show its notes in the piano roll.</li>
<li><img src="images/icons/pin.png" alt="" width="16"> <b>Set as reference track</b> pins a track so that its notes are drawn behind the notes of the track you are editing &ndash; useful when tuning harmonies to a lead.</li>
<li><b>Use DAW Track Color</b> colours notes with each track's colour from the host.</li>
</ul>
<p>When the session has no tracks, the panel shows <i>No tracks</i>.</p>
""")

# ---------------------------------------------------------------- Projects
page("projects", "Opening, Saving and Exporting", """
<h2 id="open">Opening files</h2>
<p><b>File &rsaquo; Open</b> (<kbd>Cmd</kbd>+<kbd>O</kbd>), dragging a file onto the window, or passing a file on the command line
(<code>PitchNet path/to/take.wav</code>) opens it.</p>
<table><tr><th>Type</th><th>Extensions</th></tr>
<tr><td>Audio</td><td><code>.wav</code> <code>.mp3</code> <code>.flac</code> <code>.aiff</code> <code>.ogg</code> <code>.m4a</code></td></tr>
<tr><td>PitchNet project</td><td><code>.pitchnet</code></td></tr>
<tr><td>MIDI reference</td><td><code>.mid</code> <code>.midi</code> (from <a href="midi.html#reference">Parameters &rsaquo; MIDI</a>)</td></tr>
</table>
<p>Audio at any sample rate is accepted; PitchNet converts internally with high-quality band-limited resampling (r8brain).</p>

<h2 id="save">Saving projects</h2>
<p><b>File &rsaquo; Save Project</b> (<kbd>Cmd</kbd>+<kbd>S</kbd>) writes a <code>.pitchnet</code> file with the analysis, all your
edits and the project settings. The project refers to the original audio file; it does not copy it.
When you close a window with unsaved changes, PitchNet asks <i>Save changes to this project before closing?</i></p>

<h3>If the audio has moved or changed</h3>
<ul>
<li><b>Audio file missing</b> &ndash; PitchNet asks whether you want to <b>Locate Audio</b>. Choose the file in its new place.</li>
<li><b>Audio file changed</b> &ndash; the saved audio's fingerprint does not match the file. Choose <b>Use Saved Edits</b> to apply your
edits anyway, or <b>Re-analyze</b> to start again from the new audio.</li>
</ul>

<h2 id="export">Exporting audio</h2>
<p><b>File &rsaquo; Export Audio&hellip;</b> (<kbd>Cmd</kbd>+<kbd>E</kbd>) opens <b>Export Settings</b>:</p>
<table><tr><th>Setting</th><th>Choices</th></tr>
<tr><td>Format</td><td>WAV, FLAC, AIFF, OGG</td></tr>
<tr><td>Sample Rate</td><td>The project rate or a common rate</td></tr>
<tr><td>Bit Depth</td><td>16, 24 or 32 bit</td></tr>
<tr><td>Channels</td><td>Mono or stereo</td></tr>
</table>
<p>Click <b>Export</b> and choose where to save. PitchNet renders any pending edits first, then writes the file and confirms
<i>Audio exported successfully</i>.</p>

<h2 id="export-midi">Exporting MIDI</h2>
<p>See <a href="midi.html#export">MIDI reference and export</a>.</p>
""")

# ---------------------------------------------------------------- Plug-in
page("plugin", "Using the Plug-in", f"""
<p>The PitchNet plug-in works in two ways, depending on your host.</p>

<h2 id="ara">ARA mode</h2>
<p>In hosts that support ARA 2 (for example Studio One, Cubase and Logic Pro), add PitchNet as an ARA
extension on an audio track or clip. PitchNet reads the audio directly from the host: there is nothing to record, and edits
follow the clips when you move, copy or trim them.</p>
<ul>
<li>{ico('region')} <b>Clip / Track mode</b> in the toolbar switches between editing the selected clip and seeing every clip on the track.</li>
<li>Tempo, time signature and (where the host provides them) key and chords come from the host.</li>
<li>The <a href="tracks.html">Tracks panel</a> lists every track using PitchNet; pin one as a reference to tune against it.</li>
<li>Edits are saved in the host session.</li>
</ul>

<h2 id="capture">Capture mode (hosts without ARA)</h2>
<ol>
<li>Insert PitchNet as an effect on the audio track.</li>
<li>Click {ico('record')} <b>Capture</b> and play the part in the host. PitchNet records what it hears.</li>
<li>Stop playback. PitchNet analyses the captured take and shows its notes.</li>
<li>Edit as usual. When the host plays that part of the timeline, PitchNet plays the edited audio.</li>
</ol>
<div class="warn"><b>Limits:</b> a capture can be up to 5 minutes long; record the rest in a new take. If the host jumps on the timeline
during a capture, recording pauses &ndash; stop playback to analyse what was captured.</div>
""")

# ---------------------------------------------------------------- Settings
page("settings", "Settings", """
<p>Open Settings with <b>PitchNet &rsaquo; Settings</b> (macOS) or <b>File &rsaquo; Settings</b> (Windows/Linux), or press <kbd>Cmd</kbd>+<kbd>,</kbd>.
In the standalone app it holds the audio output settings; in the plug-in the host handles audio. Close it with the <b>X</b> button.</p>
[[fig:settings.png|The Settings window in the standalone app.]]

<table><tr><th>Setting</th><th>What it does</th></tr>
<tr><td><b>Audio Driver</b></td><td>CoreAudio on macOS; Windows Audio, DirectSound or ASIO (ASIO builds) on Windows; ALSA on Linux.</td></tr>
<tr><td><b>Output Device</b></td><td>The audio interface or speakers to play through. <i>System Default</i> follows your system.</td></tr>
<tr><td><b>Sample Rate</b></td><td>The device sample rate.</td></tr>
<tr><td><b>Buffer Size</b></td><td>In samples. Smaller is more responsive; larger avoids dropouts on slower computers.</td></tr>
<tr><td><b>Channels</b></td><td>Mono or Stereo output.</td></tr>
</table>

<h2>Other settings</h2>
<ul>
<li>The inference <b>Device</b> (CPU, CoreML, DirectML or CUDA) is chosen in <a href="parameters.html#rendering">Parameters &rsaquo; Rendering</a>.
CoreML uses the Apple Neural Engine or GPU and is the best option on Macs; DirectML uses the GPU through DirectX 12 and works with most
GPUs on Windows; CUDA is fastest with an NVIDIA GPU; the CPU is the most compatible. The device cannot be changed while inference is running.</li>
<li>The interface <b>Language</b> is in <a href="parameters.html#language">Parameters &rsaquo; Language</a>.</li>
</ul>
""")

# ---------------------------------------------------------------- Shortcuts
page("shortcuts", "Keyboard Shortcuts", """
<p>On Windows and Linux, use <kbd>Ctrl</kbd> for <kbd>Cmd</kbd> and <kbd>Alt</kbd> for <kbd>Option</kbd>.</p>
<h2>File</h2>
<table><tr><th>Shortcut</th><th>Command</th></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>O</kbd></td><td>Open</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>S</kbd></td><td>Save Project</td></tr>
<tr><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>S</kbd></td><td>Save Project As&hellip;</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>E</kbd></td><td>Export Audio&hellip;</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>,</kbd></td><td>Settings</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>Q</kbd></td><td>Quit</td></tr>
</table>
<h2>Edit</h2>
<table><tr><th>Shortcut</th><th>Command</th></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>Z</kbd></td><td>Undo</td></tr>
<tr><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>Z</kbd>, <kbd>Cmd</kbd>+<kbd>Y</kbd></td><td>Redo</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>A</kbd></td><td>Select All</td></tr>
<tr><td><kbd>Cmd</kbd>+<kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd></td><td>Cut / Copy / Paste notes (polyphonic takes)</td></tr>
</table>
<h2>Transport</h2>
<table><tr><th>Shortcut</th><th>Command</th></tr>
<tr><td><kbd>Space</kbd></td><td>Play / Pause</td></tr>
<tr><td><kbd>Esc</kbd></td><td>Stop</td></tr>
<tr><td><kbd>Home</kbd></td><td>Go to Start</td></tr>
<tr><td><kbd>End</kbd></td><td>Go to End</td></tr>
</table>
<h2>Tools and view</h2>
<table><tr><th>Shortcut</th><th>Command</th></tr>
<tr><td><kbd>1</kbd></td><td>Main Tool</td></tr>
<tr><td><kbd>2</kbd></td><td>Note Separation Tool</td></tr>
<tr><td><kbd>3</kbd></td><td>Pitch Drawing Tool</td></tr>
<tr><td><kbd>4</kbd></td><td>Timing Tool</td></tr>
<tr><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>D</kbd></td><td>Show Delta Pitch</td></tr>
<tr><td><kbd>Shift</kbd>+<kbd>Cmd</kbd>+<kbd>B</kbd></td><td>Show Base Pitch</td></tr>
</table>
<p>For mouse actions with modifier keys, see <a href="mouse.html">Mouse and trackpad</a>.</p>
""")

# ---------------------------------------------------------------- Mouse
page("mouse", "Mouse and Trackpad", """
<table><tr><th>Where</th><th>Action</th><th>Result</th></tr>
<tr><td rowspan="6">Note (Main Tool)</td><td>Click</td><td>Select the note</td></tr>
<tr><td><kbd>Cmd</kbd>-click</td><td>Add to / remove from the selection</td></tr>
<tr><td>Drag up/down</td><td>Change pitch (snapped if Drag Snap is on)</td></tr>
<tr><td><kbd>Option</kbd>-drag</td><td>Change pitch with snapping inverted</td></tr>
<tr><td>Double-click</td><td>Snap to pitch centre / scale / MIDI reference</td></tr>
<tr><td>Right-click</td><td>Select it and open Restore Original and the tool menu</td></tr>
<tr><td rowspan="3">Empty canvas</td><td>Drag</td><td>Box-select notes</td></tr>
<tr><td><kbd>Cmd</kbd>-drag</td><td>Add a box to the selection</td></tr>
<tr><td><kbd>Option</kbd>-drag</td><td>Pan the view</td></tr>
<tr><td>Note handle</td><td>Drag / double-click</td><td>Change / reset that handle (see <a href="note-handles.html">Note Handles</a>)</td></tr>
<tr><td>Pitch handles</td><td>Hold <kbd>Option</kbd></td><td>Show Left/Right Slope handles</td></tr>
<tr><td>Breath note</td><td>Drag / double-click</td><td>Change / reset the breath level</td></tr>
<tr><td>Note (Separation Tool)</td><td>Click inside / on a boundary</td><td>Split / merge notes</td></tr>
<tr><td>Note edge (Timing Tool)</td><td>Drag</td><td>Move or stretch in time; <kbd>Option</kbd> inverts grid snap</td></tr>
<tr><td>Timeline</td><td>Click</td><td>Move the playhead</td></tr>
<tr><td>Loop bar</td><td>Drag</td><td>Set the loop range</td></tr>
<tr><td>Timeline or canvas</td><td>Middle-button drag</td><td>Scrub</td></tr>
<tr><td>Anywhere in the piano roll</td><td>Scroll / <kbd>Shift</kbd>+scroll</td><td>Scroll vertically / horizontally</td></tr>
<tr><td>Anywhere in the piano roll</td><td><kbd>Cmd</kbd>+scroll</td><td>Zoom both ways around the pointer</td></tr>
<tr><td>Piano keys / timeline</td><td>Scroll</td><td>Zoom vertically / horizontally</td></tr>
<tr><td>Sliders and value fields</td><td>Double-click</td><td>Reset to default</td></tr>
</table>
""")

# ---------------------------------------------------------------- Languages
page("languages", "Languages", """
<p>PitchNet's interface is available in English, Espa&ntilde;ol, &#26085;&#26412;&#35486;, &#54620;&#44397;&#50612;, Portugu&ecirc;s,
&#1056;&#1091;&#1089;&#1089;&#1082;&#1080;&#1081;, &#31616;&#20307;&#20013;&#25991; and &#32321;&#39636;&#20013;&#25991;.</p>
<p>Choose the language in <a href="parameters.html#language">Parameters &rsaquo; Language</a>. <b>System default</b> follows your
operating system's language. The change applies immediately.</p>
<p>Translations are JSON files in <code>Resources/lang</code>. To add or improve a translation, copy <code>en.json</code>,
translate the values and open a pull request on <a href="https://github.com/SessionLoops/PitchNet">GitHub</a>.</p>
""")

# ---------------------------------------------------------------- Glossary
page("glossary", "Glossary", """
<dl>
<dt id="analysis"><b>Analysis</b></dt><dd>Detecting the pitch, voicing and notes of a recording. See <a href="analysis.html">How analysis works</a>.</dd>
<dt><b>ARA</b></dt><dd>Audio Random Access: a plug-in extension that lets PitchNet read audio straight from the host. See <a href="plugin.html#ara">ARA mode</a>.</dd>
<dt><b>Base pitch</b></dt><dd>The smooth curve made by the notes' pitches, without the detail inside each note.</dd>
<dt><b>Breath note</b></dt><dd>A breath between notes, drawn as its own note so its level can be changed.</dd>
<dt><b>Cent</b></dt><dd>One hundredth of a semitone.</dd>
<dt><b>Delta pitch</b></dt><dd>The difference between the pitch curve and the base pitch &ndash; drift, vibrato and other detail.</dd>
<dt><b>F0</b></dt><dd>Fundamental frequency: the pitch of a voiced sound.</dd>
<dt><b>Formant</b></dt><dd>Resonances of the voice that give it its tone colour and vowel sound, independent of pitch.</dd>
<dt><b>Frame</b></dt><dd>The unit of analysis: 512 samples (about 11.6&nbsp;ms at 44.1&nbsp;kHz).</dd>
<dt><b>GAME</b></dt><dd>The neural note-segmentation model PitchNet uses to split pitch into notes.</dd>
<dt id="note"><b>Note</b></dt><dd>A stretch of audio at one pitch, shown as a blob in the piano roll. Notes are what you edit.</dd>
<dt><b>Pitch Center</b></dt><dd>The average pitch of a note, which Double Click Snap and Correct Pitch move to a target.</dd>
<dt><b>Pitch Drift</b></dt><dd>Slow pitch wandering within a note.</dd>
<dt><b>Pitch Modulation</b></dt><dd>Faster pitch movement within a note, mostly vibrato.</dd>
<dt><b>Polyphonic</b></dt><dd>Audio with several notes at once. See <a href="polyphonic.html">Polyphonic editing</a>.</dd>
<dt><b>PSOLA</b></dt><dd>Pitch-Synchronous Overlap-Add, the DSP method behind the Classic engine.</dd>
<dt><b>Sibilance</b></dt><dd>Hissing consonants such as &ldquo;s&rdquo; and &ldquo;sh&rdquo;.</dd>
<dt><b>Take</b></dt><dd>One analysed recording (a file, a captured passage, or an ARA clip).</dd>
<dt><b>Vocoder</b></dt><dd>A model that turns a spectrum and pitch back into audio. PitchNet uses PC-NSF-HiFiGAN for AI Resynthesis.</dd>
</dl>
""")

# ---------------------------------------------------------------- Troubleshooting
page("troubleshooting", "FAQ and Troubleshooting", """
<h2>Frequently asked questions</h2>
<h3>Does PitchNet change my original file?</h3>
<p>No. Edits are stored in the project (or the DAW session) and applied when you play or export.</p>
<h3>Why are some parts of a note hatched, without a pitch line?</h3>
<p>Those frames are breaths or sibilants. They keep the original recording, so there is no pitch to show. See <a href="breaths-sibilance.html">Breaths and sibilants</a>.</p>
<h3>Two syllables became one note (or one note became two).</h3>
<p>Use the <a href="edit-tools.html#split">Note Separation Tool</a> (<kbd>2</kbd>): click inside a note to split it, click a boundary to merge.</p>
<h3>Large pitch changes sound rough.</h3>
<p>Switch the engine to <b>AI Resynthesis</b> in <a href="parameters.html#rendering">Parameters &rsaquo; Rendering</a>.</p>
<h3>Editing is slow.</h3>
<p>Choose a GPU device (CoreML, DirectML or CUDA) in <a href="parameters.html#rendering">Parameters &rsaquo; Rendering</a>, or use the <b>Classic</b> engine, which needs no model.</p>

<h2>Error messages</h2>
<table><tr><th>Message</th><th>What to do</th></tr>
<tr><td>Missing model file / Model load failed / GAME models were not found</td><td>The installation is incomplete. Reinstall PitchNet. If you built it yourself, check that <code>Resources/models</code> contains every model (see <a href="building.html">Building</a>).</td></tr>
<tr><td>Failed to extract pitch (F0)</td><td>Choose the CPU device in <a href="parameters.html#rendering">Parameters &rsaquo; Rendering</a> and try again. PitchNet already retries on the CPU once; if that also fails, the log has details.</td></tr>
<tr><td>Inference failed</td><td>Check the inference device settings (CPU/CUDA/DirectML) or model compatibility.</td></tr>
<tr><td>Audio file missing</td><td>Click <b>Locate Audio</b> and choose the file in its new place.</td></tr>
<tr><td>Audio file changed</td><td>Choose <b>Use Saved Edits</b> or <b>Re-analyze</b>. See <a href="projects.html#save">Saving projects</a>.</td></tr>
<tr><td>Project opened, but applying saved pitch edits failed</td><td>Click <b>Re-analyze</b> to rebuild the pitch data.</td></tr>
<tr><td>No notes to export</td><td>Open and analyse audio before exporting MIDI.</td></tr>
<tr><td>Could not open file for writing / Failed to write audio data</td><td>Choose another export folder, or check that the file is not open in another app.</td></tr>
<tr><td>5-minute capture limit reached</td><td>Stop playback to analyse; record the remainder in a new take.</td></tr>
</table>

<h2>Log files</h2>
<p>PitchNet writes a log for each session to <code>~/Library/Logs/PitchNet</code> on macOS, <code>%APPDATA%\\PitchNet\\Logs</code> on Windows
and <code>~/.config/PitchNet/logs</code> on Linux. Attach the log when you
<a href="https://github.com/SessionLoops/PitchNet/issues">report a problem</a>.</p>
""")

# ---------------------------------------------------------------- Building
page("building", "Building from Source", """
<p>PitchNet is built with CMake. You need CMake 3.22+, a C++17 compiler and Git.</p>
<table><tr><th>Platform</th><th>Requirements</th></tr>
<tr><td>Windows</td><td>Visual Studio 2019+ with the C++ workload, Windows SDK</td></tr>
<tr><td>macOS</td><td>Xcode and the command line tools</td></tr>
<tr><td>Linux</td><td>GCC or Clang, ALSA development libraries</td></tr>
</table>
<pre><code>git clone https://github.com/SessionLoops/PitchNet.git
cd PitchNet
git submodule update --init --recursive
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release</code></pre>
<p>On macOS you can also generate an Xcode project with <code>cmake -B build-xcode -G Xcode</code> and run the <b>PitchNet</b> scheme.</p>

<h2>Options</h2>
<table><tr><th>Flag</th><th>Effect</th></tr>
<tr><td><code>-DUSE_CUDA=ON</code></td><td>CUDA execution provider (Windows)</td></tr>
<tr><td><code>-DUSE_DIRECTML=ON</code></td><td>DirectML execution provider (Windows)</td></tr>
<tr><td><code>-DUSE_ASIO=ON</code></td><td>ASIO audio (Windows)</td></tr>
<tr><td><code>-DONNXRUNTIME_VERSION=&hellip;</code></td><td>Override the ONNX Runtime version (downloaded during configure)</td></tr>
<tr><td><code>AAX_SDK_PATH</code></td><td>Builds AAX when set to a valid AAX SDK</td></tr>
</table>
<p>CUDA and DirectML cannot be enabled together. ARA is enabled when the <code>third_party/ARA_SDK</code> submodule is present.</p>

<h2>Models</h2>
<p>The build expects these files in <code>Resources/models</code>: <code>pc_nsf_hifigan.onnx</code>, <code>rmvpe.onnx</code>,
<code>fcpe.onnx</code> (optional), the <code>GAME/</code> folder, <code>cent_table.bin</code> and <code>mel_filterbank.bin</code>.
The optional <code>basic_pitch_nmp.onnx</code> and <code>yamnet.onnx</code> are downloaded by CMake when missing.</p>
""")

# ---------------------------------------------------------------- Credits
page("credits", "Credits and License", """
<h2>License</h2>
<p>PitchNet is free software, distributed under the <a href="https://www.gnu.org/licenses/agpl-3.0.html">GNU Affero General Public License, version 3</a>.
When you convey copies or modified versions, keep the copyright, license and attribution notices, and make the corresponding source available as the AGPLv3 requires.</p>

<h2>Origin</h2>
<p>PitchNet is a modified derivative of <a href="https://github.com/KCKT0112/HachiTune">HachiTune</a>, developed by the HachiTune contributors,
with later changes by Session Loops and other contributors.</p>

<h2>Models and libraries</h2>
<ul>
<li><a href="https://github.com/openvpi/GAME">GAME</a> &ndash; note segmentation, by OpenVPI.</li>
<li><a href="https://github.com/openvpi/vocoders">PC-NSF-HiFiGAN</a> &ndash; neural vocoder, by OpenVPI.</li>
<li><a href="https://github.com/yxlllc/RMVPE">RMVPE</a> &ndash; pitch estimation, by yxlllc.</li>
<li>FCPE and SwiftF0 &ndash; pitch estimation.</li>
<li><a href="https://github.com/spotify/basic-pitch">Basic Pitch</a> &ndash; note evidence for polyphonic analysis, by Spotify (Apache-2.0).</li>
<li>YAMNet &ndash; input-type classification.</li>
<li><a href="https://github.com/avaneev/r8brain-free-src">r8brain-free-src</a> &ndash; sample-rate conversion, by Aleksey Vaneev (MIT).</li>
<li><a href="https://juce.com">JUCE</a> &ndash; application and plug-in framework; ONNX Runtime &ndash; model inference.</li>
</ul>
<p>The full list is in <b>PitchNet &rsaquo; About PitchNet</b> and in <code>THIRD_PARTY_NOTICES.txt</code>.</p>

<h2>About this manual</h2>
<p>The layout of this manual follows the <a href="https://manual.audacityteam.org/">Audacity Reference Manual</a>.
Corrections are welcome as pull requests to the <code>docs/manual</code> folder on GitHub.
The vocal in the screenshots is from the <a href="https://zenodo.org/record/5578807">vocadito</a> dataset (Bittner et al.).</p>
""")

# ---------------------------------------------------------------- Navigation
NAV = [
    ("Manual", ["index"]),
    ("Using PitchNet", ["getting-started", "tutorial-first-edit", "projects", "plugin"]),
    ("PitchNet Window", ["menus", "toolbar", "edit-tools", "piano-roll", "parameters", "tracks"]),
    ("Editing", ["note-handles", "correct-pitch", "pitch-drawing", "timing", "breaths-sibilance"]),
    ("Analysis & Sound", ["analysis", "polyphonic", "rendering", "chord-track", "midi"]),
    ("Customizing", ["settings", "shortcuts", "mouse", "languages"]),
    ("Reference", ["glossary", "troubleshooting", "building", "credits"]),
]
