# 12 Render profiling: why the GPU canvas made the window slower, and where the application spends its time

`11-rendering.md` measured that drawing the canvas on the GPU removed almost all of the canvas's CPU drawing and still
made a repaint and a resize of the window slower than the CPU path. This page says why, what was changed, what was not,
and where the rest of the application spends its time.

Everything here was measured on one machine: Windows 11, NVIDIA GeForce RTX 4090, Direct3D 11, a 100 Hz display, Qt
6.12, a Release build with Tracy (`windows-msvc-tracy-gui`) with a profiler attached. Other sessions were building and
testing on the same machine, so each figure is a range over two or three runs, and the software figures themselves
moved by about 2 ms between the first and the last hour. Nothing else was measured: see
[What was not measured](#what-was-not-measured).

## The short answer

There were five causes. The first, second and fifth are how Qt presents a window that holds a GPU widget; the third and
fourth were ours.

1. **The window waited for the display.** A window that holds a GPU widget is presented through the graphics API, and
   by default each present waits for the next refresh of the screen with the GUI thread stopped. One repaint of
   anything in the window cost one refresh (10 ms at 100 Hz). A resize step presents the window 2.2 times, so it cost
   about 18 ms of waiting on top of the work. A window drawn by the CPU never waits. **Fixed**: Studio now asks for no
   wait (swap interval 0) when the canvas is drawn on the GPU.
2. **Qt's default Direct3D swap chain is slow to resize and stalls.** With the "flip" swap chain a resize step stayed 4
   to 5 ms slower than the CPU path, and about one present in a hundred took 40 to 190 ms, during a resize, a node move
   or heavy traffic. The older copying swap chain (`QT_D3D_NO_FLIP=1`) does neither. **Fixed**: Studio asks for it.
3. **Every frame of the canvas uploaded the canvas's part of the window again.** The view repainted its (empty)
   viewport to ask the GPU canvas for a frame, and Qt then repainted the overlay widgets and sent that rectangle of the
   window to the GPU: 1.2 to 1.5 ms a frame, more than the frame itself. **Fixed** for frames the scene asks for.
4. **Moving one node drew every node again.** The texture of an item was thrown away when any changed rectangle touched
   it, and every link was given its geometry again on every move, so a move drew all nodes and links again on the CPU
   and uploaded them: slower than the CPU path. **Fixed**: an item says itself when its drawing changed.
5. **Reading the GPU's time slowed the frames it timed.** With `QSG_RHI_PROFILE=1` the canvas waits for the GPU in every
   frame: 7 to 9 ms a frame while the window is resized, and the worst steps of 120 to 560 ms. The bench always set it,
   and Studio set it whenever the profiler was recording at start-up, so the numbers in `11-rendering.md` include it. A
   Studio that is not being profiled never had this cost. **Fixed**: GPU timing is now asked for separately.

With all five, the GPU path is as fast as the CPU path or faster in every step measured in the small window. In the
large window a node move is 0.1 ms slower. The GPU path is 220 to 250 ms slower to show its first frame.

## Before and after

`vss_gui_render_bench --size=1280x800 --ports=4` (canvas 528 x 461, 8 ports, 4 links), milliseconds per step. A step is
the time until the event loop has handled the change, including presenting the window. Median of 300 steps (240 for
resize); the range is over three or more runs.

| Step | Software (CPU) | GPU, as merged in PR 200 | GPU, now |
|---|---|---|---|
| New frame of the canvas | 2.0 to 2.9 | 10.0 | 0.19 to 0.22 |
| Pan (scroll bars showing) | 1.4 to 1.9 | 10.0 | 0.63 to 0.65 |
| Move one node | 1.7 to 2.2 | 10.0 | 1.3 to 1.4 |
| Resize the window, median | 12.5 to 16.0 | 40.2 to 40.4 | 12.2 to 12.9 |
| Resize the window, mean | 11.0 to 14.3 | 37.2 to 41.1 | 10.9 to 11.6 |
| Resize the window, slowest step | 22 to 40 | 80 to 390 | 26 to 29 |
| Canvas drawing on the CPU in a move | 1.0 to 1.3 | 1.7 | 0.09 to 0.10 |
| First frame shown after start | 364 to 420 | 590 to 680 | 590 to 620 |

"As merged" is the same build with the old behaviour switched back on (the wait for the display, the viewport repaint,
the old texture rule, the flip swap chain) and no GPU timing, which is what a Studio that is not being profiled did.
With GPU timing on, as the first measurements were taken, the same build gave 13 to 15 ms for a frame and a mean of 46
to 50 ms for a resize step with worst steps of 150 to 560 ms.

The "now" runs were taken last, back to back with software runs whose resize median was 12.5 to 13.9 ms.

A larger window (`--size=1920x1080 --ports=20`, 40 ports):

| Step | Software | GPU, now |
|---|---|---|
| New frame of the canvas | 3.2 to 3.6 | 0.25 |
| Pan | 3.3 to 3.9 | 0.77 to 0.83 |
| Move one node | 0.47 to 0.55 | 0.61 to 0.63 |
| Resize, median | 17.4 to 20.3 | 16.3 to 17.5 |

## The evidence

The traces are `tracy-capture` captures of the bench with `--profile`. `tools/tracy_phases.py` reads the unwrapped
export and gives each zone its self time, which is where time inside Qt shows: the self time of the top-level window's
`UpdateRequest` and `Expose` events is the compose and present of the window, and the self time of
`gui.render.widget` is Qt's part of the canvas widget's frame.

### A repaint (300 steps, total self time in ms)

| Zone | Software | GPU, waiting | GPU, no wait | GPU, no wait, frame asked of the canvas |
|---|---:|---:|---:|---:|
| `ShellWindow / UpdateRequest` (compose and present) | 142 | 2714 | 372 to 500 | 40 to 52 |
| `gui.render.widget` (the canvas widget, all of it) | | 88 | 51 to 56 | 40 to 66 |
| `gui.paint.node` (CPU drawing of nodes) | 511 | 0 | 0 | 0 |
| Step, median | 2.8 | 10.0 | 1.7 to 2.1 | 0.23 to 0.37 |

2714 ms over 300 presents is 9.0 ms each: the step is one refresh of a 100 Hz display, whatever is drawn. Qt's source
agrees (`qrhid3d11.cpp`: `beginFrame` waits on the swap chain's frame-latency object and `endFrame` calls
`Present(1)`; `qbackingstorerhisupport.cpp` turns a swap interval of 0 in the window's format into `NoVSync`).
`QT_D3D_MAX_FRAME_LATENCY=0` alone did not help (a step stayed at 10 to 15 ms): the present still waits.

### A resize (240 steps, total self time in ms), flip swap chain

| Zone | Software | GPU, waiting | GPU, no wait | GPU, waiting, GPU timing on | GPU, no wait, GPU timing on |
|---|---:|---:|---:|---:|---:|
| `QWidgetWindow / Expose` (308 of them) | 168 to 179 | 4225 | 479 to 587 | 1686 | 661 |
| `ShellWindow / UpdateRequest` (215) | 428 to 526 | 1809 | 1075 to 1247 | 1486 | 1250 |
| `gui.render.widget` self (590 to 680 canvas frames) | | 234 | 175 to 206 | 5377 | 4127 |
| `gui.render.upload` (item textures made again) | | 22 | 16 to 21 | 20 | 21 |
| Step, mean | 13 to 16 | 37 to 41 | 17 to 21 | 46 to 50 | 33 to 37 |

Read across:

- **Waiting**: the two flush events go from about 1800 ms to 6000 ms, 18 ms a step. A resize step flushes the window
  2.2 times (0.9 `UpdateRequest` and 1.3 `Expose`) and each waited.
- **GPU timing**: the canvas widget's own time goes from 0.3 ms a frame to 7 to 9 ms a frame, outside our `render`
  (`gui.render.frame` stays at 0.5 ms). With timing on, the wait for the display also moves there. This is the 123 to
  510 ms "slowest step" of `11-rendering.md`.
- **The flip swap chain**: with no wait and no timing the flush events still cost 1550 to 1830 ms against 600 to 700 ms
  in software, 4 to 5 ms a step, and the slowest steps were 36 to 190 ms against 22 to 40 ms.

### The swap chain

Three runs each, back to back, no wait for the display, no GPU timing:

| | Software | GPU, flip swap chain (Qt's default) | GPU, copying swap chain |
|---|---|---|---|
| Resize, median | 12.5 to 13.9 | 17.2 to 17.7 | 12.2 to 12.9 |
| Resize, slowest step | 22 to 25 | 50 to 118 | 26 to 29 |
| Move, slowest step | 2.7 to 4.9 | 65 to 158 | 2.2 to 3.3 |
| Pan, median | 1.4 | 1.5 | 0.64 |
| Events over 30 ms after start-up, per run | 0 | 2 to 12 | 0 |

The slow presents with the flip swap chain came in clusters, often 66 ms long, with the wait for the display or
without it, and their self time was in the window's flush (inside Qt and the driver). Their cause inside Direct3D was
not found; the copying swap chain does not have them. In the larger window the GPU's flush is then cheaper than the
CPU's (`UpdateRequest` and `Expose` self time 737 ms against 997 ms over 240 resize steps).

### The hypotheses

| | Hypothesis | Verdict |
|---|---|---|
| a | The window is composed through the graphics API and the present waits for the display | **Yes, the main cause.** But Qt uploads only the changed rectangle of the window each frame, not all of it; all of it only after a resize. |
| b | The swap chain and the canvas buffers are made again on every resize step | Yes: 4 to 5 ms a step with the flip swap chain, nothing measurable with the copying one. The canvas's colour buffer is made again for every size (`gui.render.init` runs 217 times in 240 steps and takes 0.1 ms in all). |
| c | The canvas throws item textures away or draws them again on resize | No. 170 to 190 uploads in 240 steps, 16 to 22 ms in all (the 30 percent tolerance and the 120 ms settle timer work). It was true for *moving a node*, see below. |
| d | The view's paint event and `FullViewportUpdate` cause extra frames | Partly. The canvas renders 2.5 to 2.8 times per resize step (once from Qt's own resize handler, synchronously) and the window is flushed 2.2 times; each flush waited. The view's own paint is 1 ms in 240 steps. Stopping the synchronous render did not change a step, so it stays. The viewport repaint did cost an upload per frame (cause 3). |
| e | The bench | Yes, in two ways: it always turned GPU timing on, and its "pan" phase did nothing (the view was still fitting itself, so the scroll bars had no range; a step took 0.00 ms in both renderers). Both fixed. |

### Moving a node (300 steps, flip swap chain, no wait)

| | Software | GPU before | GPU with the new texture rule |
|---|---:|---:|---:|
| `gui.paint.node` calls | 2400 | 2400 | 0 |
| `gui.paint.link` calls | 1200 | 1346 | 300 |
| `gui.render.upload` (textures made) | | 3746 | 300 |
| Canvas drawing per step | 1.2 ms | 1.7 ms | 0.11 ms |
| Step, median | 2.0 ms | 4.3 to 4.8 ms | 2.0 to 2.4 ms |

Before, a move of one node drew all 8 nodes and all 4 links again and uploaded 12 textures. Now it draws the one link
whose route changed.

## What was changed

| Change | Where | Effect measured |
|---|---|---|
| Swap interval 0 when the canvas is drawn on the GPU. `VSS_RENDER_VSYNC=1` keeps the wait. | `rendering::initialise` | A frame 10.0 ms to 1.7 to 2.1 ms; a resize step 40 ms to 20 ms |
| The copying Direct3D swap chain (`QT_D3D_NO_FLIP=1`, set by Studio unless the variable is already set; `QT_D3D_NO_FLIP=0` keeps Qt's default) | `rendering::initialise` | A resize step 17.2 to 17.7 ms to 12.2 to 12.9 ms; the 40 to 190 ms presents gone |
| The view no longer repaints its viewport for a scene change or a scroll in GPU mode (`NoViewportUpdate`); the canvas is asked directly | `TopologyView::enable_gpu`, `scrollContentsBy`, `request_frame` | A frame 1.7 to 2.1 ms to 0.23 to 0.37 ms |
| An item's texture follows the item's own revision (`rendering::content_revision`), not the scene's changed rectangles; a link ignores a geometry it already has | `GpuCanvas`, `NodeItem`, `LinkItem` | A move 4.3 to 4.8 ms to 2.0 to 2.4 ms; canvas drawing 1.7 ms to 0.11 ms |
| GPU timing only when asked for: `VSS_PROFILE_GPU=1` for Studio, `--gpu-timing` for the bench | `main.cpp`, the bench | A profiled resize step 33 to 37 ms to 17 to 21 ms |
| The Monitor no longer sets the Pause button's icon for every batch of traffic (found in the whole-application profile, below) | `MonitorPanel::handle_events` | Under traffic: 380 ms of control repaints and 438 window layouts in 4 s gone; 576 batches handled in 4 s instead of 433 |
| Zones `gui.render.widget` and `gui.render.init`; the bench gets `gui.event` zones, phase zones, a median, a working pan, a move, idle and traffic phases, start-up times; `tools/tracy_phases.py` | | |

Two of these change how the window reaches the screen, and **I could not look at the screen** (the desktop must not be
captured, and `grab()` does not go through the swap chain). They need one look by a person:

- Swap interval 0 means a present may happen in the middle of a refresh. Studio's window is not full screen and is
  composed by the desktop, and it only draws when something changes, so a torn frame is not expected. If a display
  shows tearing while the canvas is panned, `VSS_RENDER_VSYNC=1` brings the wait back.
- `QT_D3D_NO_FLIP` is a switch in Qt's Direct3D backend, read when the device is made. It is not in Qt's documentation,
  so a later Qt may drop it; the bench would then show the resize step back at 17 ms. The copying swap chain is the
  older of the two ways Direct3D presents a window.

The Software setting avoids both.

## What was tried and not kept

| Tried | Result |
|---|---|
| `QT_D3D_MAX_FRAME_LATENCY=0` | No change: the present still waits |
| Not rendering the canvas from Qt's resize handler (updates off around the geometry change) | No change in a step, with or without the wait. Not kept: it would rely on Qt's internals |
| A fixed colour buffer for the canvas (`setFixedColorBufferSize`), so it is not made again on resize | No change in a step. Not usable anyway: Qt stretches the buffer to the widget |
| `QT_WIDGETS_RHI_BACKEND=d3d12`, `vulkan`, `opengl` | No effect: the renderer stayed Direct3D 11 in every run. The variable does not choose the API of a window that holds a `QRhiWidget`; `11-rendering.md` said it did and is corrected |

## Larger options

Not done. Each is a choice with a cost.

| Option | What it would give | Cost and risk |
|---|---|---|
| **A. Make the Monitor cheaper under traffic** | **Done**, see "The Monitor under heavy traffic" below: a batch of events costs the GUI thread about a fifth of what it did | |
| **B. Make a theme change cheaper** (below: 0.3 to 0.5 s with the window frozen) | The worst single stall found | Medium to large: it is Qt reading the application style sheet again for every widget. Smaller style sheets per widget class, or palette roles instead of style rules, both touch the whole theme |
| **C. Start the graphics device later** (show the first frame from the CPU and switch when the device is ready) | 220 to 250 ms off the first frame with the GPU | Medium: the window changes from CPU to GPU composition after it is shown, which Qt does by making the window's surface again; a visible flash is likely |
| **D. Give the canvas its own native window** (`WA_NativeWindow`, or a `QWindow` in `createWindowContainer`), so the rest of the window stays on the CPU flush | Little now: with the copying swap chain the GPU's flush is no dearer than the CPU's. It would matter again where no such swap chain exists (other platforms, not measured) | Medium to large. The overlays on the canvas (hint, zoom bar, empty-state text, loop note, rubber band) are widgets drawn over it; over a native window they must become native windows, which lose their see-through backgrounds, or be drawn by the canvas |
| **E. The setup check off the GUI thread** (`ports.permissions.observe_windows`, 20 ms a call) | 60 ms at start-up and 20 ms at every refresh of the readiness chip | Small to medium: the result has to come back by a queued signal |
| **F. Find the 66 ms presents of the flip swap chain** | Only matters if `QT_D3D_NO_FLIP` goes away | Needs an administrator: Tracy's sampling or an ETW trace of the present |

## The whole application

Profiles: the bench on the real desktop in both renderers (start-up, idle with 8 ports, 4 seconds of traffic with every
fixture sending 50 chunks of 256 bytes a second into its loopback and echoing what comes back, resize, move, pan), and
on the offscreen platform `vss_gui --selftest --profile` (every dialog and Settings page in both themes, 20 s),
`vss_gui_dialogs_tests` and `vss_gui_journey_tests`. The installed Studio was running on the machine, so a second
`vss_gui` could not be driven by hand; clicking through the real application was not profiled.

### By area

| Scenario | Software | GPU | Notes |
|---|---|---|---|
| Start-up: window made | 150 to 170 ms | 150 to 180 ms | |
| Start-up: first frame shown | 364 to 420 ms | 590 to 620 ms | The graphics device, the swap chain and the first frames. The first canvas frame alone is 54 to 61 ms and the first resize of the canvas 50 to 58 ms |
| Idle, 8 ports, 2 s | 0 to 2.4 % of the GUI thread | 0 to 2.4 % | Nothing repaints; the loop sleeps |
| Traffic, 4 s: GUI thread working | 66 to 95 % | 73 to 92 % (52 to 71 % when waiting for the display) | The engine routed 160 000 chunks in 4 s; the GUI thread is the limit in both |
| Traffic: longest time away from the event loop | 7 to 55 ms | 32 to 60 ms (up to 111 ms with the flip swap chain) | |
| Shutdown (hub stopping to the last zone) | 190 ms | 270 ms | Measured with the flip swap chain |

### Hotspots, ranked

"Ours" means the time is in Studio's code; "Qt" means it is inside Qt with our code only as the caller. The traffic
figures are of the GUI thread over 4 s.

| # | Where | Zone | Cost | Worst single | Whose |
|---|---|---|---|---|---|
| 1 | A theme change | `gui.theme.apply` | 308 ms mean with the window frozen | 484 ms | Qt, reading the style sheet again for every widget: 70 % is inside `setStyleSheet`, the rest is `StyleChange` and `PaletteChange` events, the dearest to scroll areas (1.7 ms each). The size of our style sheet decides it |
| 2 | The Monitor under traffic | `QWidget / Paint / qt_scrollarea_viewport` (the table) | 1150 to 1300 ms: 30 % of the thread, 2.1 ms a paint | 13 ms | Qt's table painting, calling our model's `data`. **Made cheaper**, see "The Monitor under heavy traffic" |
| 3 | The Monitor under traffic | `gui.monitor.append_events` | 520 to 700 ms (13 to 17 %) | 12 ms | Ours. **Made cheaper**, see "The Monitor under heavy traffic" |
| 4 | Events from the engine | `gui.bridge.drain` self | 520 to 680 ms (13 to 17 %) | 44 ms | Ours. **Made cheaper**, see "The Monitor under heavy traffic" |
| 5 | Start-up with the GPU | first frames | 220 to 250 ms more than software | 61 ms | Qt and the graphics driver |
| 6 | The setup check | `ports.permissions.observe_windows` | 20 ms a call on the GUI thread, three times at start-up | 30 ms | Ours (an operating-system query; 56 % of the journey tests' trace) |
| 7 | A resize step | every widget repaints | 11 to 14 ms a step | 29 ms | Qt, with ours inside: `gui.paint.port_row` 0.9 ms a step and, in software, `gui.paint.node` 1.4 ms a step |
| 8 | Opening dialogs | `gui.dialogs.share_wizard.build`, `gui.dialogs.connect_ports.build`, `gui.shell.palette.activate` | 26, 20 and 55 ms | 48, 42 and 55 ms | Ours |
| 9 | Presenting the window under traffic | `ShellWindow / UpdateRequest` self | 200 ms in software; 530 ms with the flip swap chain | 41 ms | Qt. Not measured in the small window with the copying swap chain; in the large one it was 145 ms against 202 ms in software |
| 10 | The Monitor's bar under traffic | repaints of its buttons, boxes and fields, `ShellWindow / LayoutRequest` | 380 ms + 57 ms | | Ours. **Fixed here** |

Events over their budget (`gui.slow_event`; the budget is 16 ms for painting) in the final build, bench only: in
software none over 30 ms; with the GPU two at start-up (the first canvas frame and the first canvas resize, 50 to 57
ms each, each reported once per widget above it) and none after.

### The Monitor under heavy traffic

Hotspots 2 to 4 were profiled on their own and made cheaper. The traffic phase of the bench in software, small window
(`--render=software --size=1280x800 --ports=4`), three runs before and three after, GUI thread over 4 s.

The event bridge hands the window batches of 256 events while it has a backlog, and pauses for twice as long as a batch
took, so the window is never busy for more than about a third of the time on the bridge's account. In every run, before
and after, every batch was full: the engine makes events faster than the window takes them and the bridge drops the
oldest. A cheaper batch therefore means more batches in the 4 s, and the totals have to be read with the number of
batches. The cost of a batch is the figure to compare.

| | Before | After |
|---|---|---|
| Batches of 256 events handled in 4 s | 369 to 519 | 1261 to 1365 |
| GUI thread working | 70 to 75 % | 40 to 58 % |
| Longest time away from the event loop | 32 to 65 ms | 5 to 20 ms |
| Table paint (`QWidget / Paint / qt_scrollarea_viewport`), total | 1251 to 1477 ms | 228 to 404 ms |
| Table paint, for each batch | 2.4 to 3.9 ms | 0.18 to 0.32 ms |
| Table paint, each (number of paints) | 2.2 to 3.5 ms (414 to 569) | 1.5 to 2.2 ms (151 to 180) |
| Adding rows (`gui.monitor.append_events`; after, with `gui.monitor.commit`), total | 715 to 742 ms | 239 to 275 ms |
| Adding rows, for each batch | 1.4 to 2.0 ms | 0.19 to 0.22 ms |
| The bridge's own time (`gui.bridge.drain` less `gui.bridge.emit`), total | 599 to 702 ms | 787 to 886 ms |
| The bridge's own time, for each batch | 1.4 to 1.7 ms | 0.62 to 0.67 ms |

The bridge's total went up because it handled three times the events.

What the time was, found with zones added for it (`gui.monitor.paint_cell` with the column as its text,
`gui.monitor.data`, `gui.bridge.take`, `gui.bridge.convert`, `gui.bridge.release`, and for a while one zone for each
row and each step of a row):

| Found | Measured | Changed |
|---|---|---|
| The table was painted once for every batch: 100 to 140 times a second, and with 256 new rows a batch every paint showed rows nobody had seen and nobody would see again | One paint 2.2 to 3.5 ms | `MonitorPanel` puts new rows in the table at most once every 33 ms (`kCommitIntervalMs`). Rows that arrive slower than that are shown as they arrive. Pause, Export and Export capture show what has arrived first |
| A paint is cheap in the model and dear in Qt. The model's `data` is 0.5 microseconds a call (about 6 calls for each cell). Qt's drawing of a cell is 30 to 90 microseconds, plus about 2 microseconds for each character of its text, visible or not: a 256 character preview cell is 560 microseconds, 8 times the Time cell. Only 2.6 rows are visible in the small window | Preview cell 560, Time 92, Port 59, Bytes 57, Direction 32 microseconds | `MonitorCellDelegate` hands Qt only the start of a long preview when the font is fixed-width: a quarter more characters than fill the cell, plus 16. Qt cuts that where it would have cut the whole text. Preview cell 320 to 330 microseconds as text in a cell of this width; more is saved as hex (767 characters) and in a narrow cell |
| Each row read the wall clock | 3.5 microseconds a row, 485 ms in 4 s: two thirds of `append_events` | One reading for a batch |
| Each event got a block of memory for its bytes in the bridge and one for its port name, and gave them back when the Monitor dropped its row. Under this traffic a block of this size costs 1.9 microseconds to get, 1.35 to fill and 1.6 to give back; the name costs 0.14 | Bytes 3.6 microseconds an event in `gui.bridge.convert`; dropping old rows 0.4 ms a batch at first, 2.1 microseconds a row once rows arrived faster | The bridge copies the bytes of a batch into one block and each event has a part of it (`event_payload`); the Monitor's rows keep the block alive. Port names are shared between events (`EventBridge::name_cache`). Dropping a row is now 0.6 microseconds |
| Rows were already added as one insertion for a batch, and the model's signals are cheap | `beginInsertRows` to `endInsertRows` 5 microseconds, removing 256 rows from the front 26 microseconds | Nothing |

What is left for a batch of 256 events, after: `gui.bridge.convert` 0.55 to 0.58 ms (reading the engine's events and
copying their bytes, 2.2 microseconds an event, most of it waiting for memory the engine's threads wrote), dropping old
rows 0.15 ms, the Monitor's share of a paint 0.2 to 0.3 ms, the support log 0.14 ms, taking the events from the queue
0.05 ms. Copying the bytes on the engine's thread when the event is made, while they are in that processor's cache,
would take most of `convert` off the GUI thread; it means more work under the bridge's lock on the hub's dispatcher
and was not tried.

## How to repeat it

```
tracy-capture -a 127.0.0.1 -o hw.tracy -f &
vss_gui_render_bench --render=hardware --size=1280x800 --ports=4 --profile --profile-wait=30
tracy-csvexport -u hw.tracy > hw.u.csv
python tools/tracy_phases.py hw.u.csv --gui --phase bench.resize
```

`--render=software` gives the other side. `VSS_RENDER_VSYNC=1`, `QT_D3D_NO_FLIP=0` and `--gpu-timing` each bring back
one of the costs that were removed. The bench prints the display's refresh rate and the swap interval it ran with.

## What was not measured

- Any other machine, GPU, driver or operating system: no macOS (Metal), no Linux (OpenGL or Vulkan), no AMD or Intel
  GPU, no remote desktop, no virtual machine. The wait for the display is the default of every API Qt uses, so swap
  interval 0 should matter everywhere, but that is a reading of Qt's source, not a measurement. The swap chain switch
  is Direct3D only.
- Direct3D 12, Vulkan and OpenGL on this machine: the variable that was meant to choose them had no effect.
- What the window looks like on the screen with swap interval 0 and the copying swap chain: not looked at.
- The call stacks inside Qt: Tracy's sampling needs an administrator and the Qt build here has no symbols. The time
  inside Qt was located by the self times of the events around it and by switching one thing at a time.
- The traffic phase in the small window with the final build, zone by zone (the traces of those runs were overwritten
  by the large-window runs); the step times and the bench's own figures above are from the final build.
- The real application driven by hand, and long runs (memory growth, hours of traffic).
- The Monitor's changes in a tall Monitor pane, in Hex, with a filter or a search set, with Follow off, and with the
  canvas drawn on the GPU: the bench has the small window, 2.6 rows visible, text previews, no filter and Follow on.
  The cost of a painted cell is for each cell, so a pane with 20 rows visible pays about 8 times as much for a paint.
- Whether the cut preview is the same picture in every font: the test compares it with Qt's own drawing of the whole
  text in the first fixed-width font of the machine, at 60 widths. The cut is not made in a font that is not
  fixed-width, so there the cell costs what it did.
- Why a block of memory is so dear on the GUI thread under this traffic (the heap shared with the engine's threads, or
  memory out of the cache): the profile shows where, not why.
- The `gui.gpu.frame` zone was checked to appear when asked for (2029 zones in a bench run with `--gpu-timing`, 7 to
  1083 microseconds, median 35) and not otherwise; its place on the timeline is approximate, as `11-rendering.md` says.
