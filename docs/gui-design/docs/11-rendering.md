# 11 Rendering: drawing the canvas on the GPU or the CPU

How the topology canvas is drawn, the choice between the graphics card and the processor, and what the profiler sees.
Settings, Performance, Rendering holds the choice (`screens/06-support-and-system.md`).

## Decision

The canvas is drawn by **Qt Canvas Painter** (`QCanvasPainterWidget`, a `QRhiWidget`) laid over the existing
`QGraphicsView`, when the GPU is chosen. The `QGraphicsView`, its scene and its items stay: they keep hit testing,
selection, keyboard focus, drag and drop, the overlays (zoom bar, hints) and the accessible tree. Software mode is the
code that was there before, unchanged.

The GPU canvas draws:

| What | Where it is drawn |
|---|---|
| The ground colour and the dot grid | GPU, every frame. The grid is one small texture repeated by the GPU. |
| Every node, link and label | The item's own `paint()` draws it once into an image at the current zoom and screen pixel ratio. The image is a GPU texture and the GPU places it on the screen. |
| Panning, scrolling, moving a node, zoom steps | GPU: a handful of textured rectangles. |
| The content of an item after it changes, after a theme change, or after the zoom has settled | CPU, into that item's image (text, icons, shapes). |
| Everything outside the canvas (lists, panes, dialogs, the Monitor, menus) | CPU, as before. The window is composed on the GPU as soon as it holds a GPU widget. |

A texture belongs to one item and is made again when that item says its drawing changed
(`rendering::content_revision`, which the node and link items derive from), when its size or the zoom changes, and when
the theme changes. Moving an item, or changing one that overlaps it, costs no drawing. An item that does not report a
revision is made again whenever a changed rectangle of the scene touches it.

In GPU mode the view does not repaint its viewport for a change in the scene or for a scroll
(`QGraphicsView::NoViewportUpdate`): the canvas hears of both and draws the frame. A viewport repaint would make Qt
repaint the overlay widgets and upload the canvas's part of the window again for every frame.

A texture is made at the zoom that is on screen. While the zoom is changing (a window drag that refits the canvas, the
wheel) the textures already made are drawn stretched, a little soft, and are made sharp again when the zoom has been
still for 120 ms.

## Why this and not the others

| Option | Verdict |
|---|---|
| `QGraphicsView::setViewport(QOpenGLWidget)` | Smallest change (Qt's OpenGL paint engine runs the existing `QPainter` code), but OpenGL only: deprecated on macOS, poor on remote desktops, and Direct3D, Vulkan and Metal are unused. It was the first prototype and was dropped for this reason. |
| Qt Quick (`QQuickWidget`) | Needs the items rewritten in QML and gives up the `QGraphicsItem` accessibility, tests and keyboard code. Far larger. |
| `QRhiWidget` with own shaders | Same rewrite of items, plus our own text and path renderer. |
| **Qt Canvas Painter on `QRhiWidget`** | Uses QRhi, so Direct3D 11 and 12, Vulkan, Metal and OpenGL by platform, with no item rewrite when the items are drawn into textures. Costs one new Qt module and a 6.11 minimum. |

Qt Canvas Painter is a supported module from Qt 6.11; the shared Qt install and CI need the `qtcanvaspainter` module.
`QRhiWidget` and `QCanvasPainterWidget` have no CPU backend, so software mode is the plain `QGraphicsView` painting.

## Choosing the renderer

`ui/renderMode` is `auto` (default), `hardware` or `software`.

| Setting | Behaviour |
|---|---|
| Automatic | GPU when a graphics device exists and is not itself a CPU renderer (WARP, llvmpipe); otherwise CPU. |
| Hardware (GPU) | GPU whenever a device exists, even a CPU renderer (for diagnosis). |
| Software (CPU) | CPU. No GPU widget is created. |

The environment variable `VSS_RENDER` and the command-line option `--render=auto|hardware|software` give the same choice
for one run. The command line wins over the environment, which wins over the saved setting. The self-test, the screenshot
run and the offscreen and minimal platforms are always software.

A change in Settings applies at the next start, and the page says so. The decision is made in
`rendering::initialise` (the request) and `rendering::confirm_gpu` (once the canvas has a device), and the rule itself
is the pure function `rendering::decide`, which is unit tested without a GPU.

### Falling back

If the canvas cannot get a device, the device is a CPU renderer in Automatic, or the device is lost while running, the
canvas drops its GPU layer and the `QGraphicsView` paints as before, in the same session. The window shows the reason in
the status bar for ten seconds and writes it to the event log; stderr gets a line (`vss_gui: rendering in software
(asked for hardware by setting): ...`) and a profiler message `gui.render_mode`. About, Environment and `environment.txt`
carry a `Renderer` line (for example `Hardware (GPU): NVIDIA GeForce RTX 4090, D3D11`).

The graphics API is the platform default of `QRhiWidget` (Direct3D 11 on Windows, Metal on macOS, OpenGL on Linux).
`QT_WIDGETS_RHI_BACKEND` does not change it: on Windows the renderer stayed Direct3D 11 with every value tried.

### Presenting the window

A window that holds a GPU widget is presented through the graphics API. Two of Qt's defaults made that slower than the
CPU path, and Studio changes both when the canvas is drawn on the GPU (`rendering::initialise`, before the window is
made). `12-render-profiling.md` has the measurements.

| Default in Qt | What Studio asks for | To get Qt's default back |
|---|---|---|
| Each present waits for the next refresh of the display, with the GUI thread stopped (swap interval 1) | No wait (swap interval 0), as for a window drawn by the CPU | `VSS_RENDER_VSYNC=1` |
| Direct3D: the "flip" swap chain, which was slow to resize and now and then held a present for 40 to 190 ms | The copying swap chain (`QT_D3D_NO_FLIP=1`, unless the variable is already set) | `QT_D3D_NO_FLIP=0` |

## Profiling

All behind the existing run-time switch; with the profiler dormant the new code adds one relaxed load per zone.

| Zone, plot or message | Meaning |
|---|---|
| `gui.render.widget` | the canvas widget's whole paint: Qt's buffers and offscreen frame around `gui.render.frame` |
| `gui.render.init` | Qt making the canvas's buffers again (once per size) |
| `gui.render.frame` | `GpuCanvas::render`: the CPU time of one GPU frame |
| `gui.render.draw`, `.dots`, `.items` | recording the draw calls, the grid, the items |
| `gui.render.upload` | an item drawn into a new texture (the CPU part that remains) |
| `gui.render.sync` | invalidating textures from the scene's changed regions (items that report no revision), dropping those of deleted items |
| `gui.render.view` | the view's own paint event (a request for the next GPU frame) |
| `gui.gpu.frame` | GPU time of a frame, from QRhi (`lastCompletedGpuTime`), in a Tracy GPU context named `gui.canvas`. Only with `VSS_PROFILE_GPU=1` |
| `gui.render.item_textures`, `gui.render.texture_bytes` | plots |
| `gui.render_mode` | message at start-up and at a fallback |

QRhi gives one duration per frame, not a start time, so the `gui.gpu.frame` zone ends when the duration is read and
starts that long before: its length is the GPU's, its place on the timeline is within a frame or two. It uses Tracy's C
API for custom GPU contexts, so the same code serves every graphics API. Qt only measures GPU time when it is asked to
at device creation (`QSG_RHI_PROFILE=1`), and reading it makes the canvas wait for the GPU in every frame: 7 to 9 ms a
frame while the window is resized. So it is off unless `VSS_PROFILE_GPU=1` is set and the profiler is already recording
at start-up; the CPU zones need neither. Nothing is sent anywhere: the listener is the existing one, on this computer
only.

## Measurements

`vss_gui_render_bench` (Release with Tracy, Windows 11, NVIDIA GeForce RTX 4090, Direct3D 11, a 100 Hz display, Qt
6.12). A step is the time until the event loop has handled the change, which includes composing and presenting the
window. Median of 300 steps (240 for resize), range over three runs.

| Canvas | | New frame | Pan | Move a node | Resize step |
|---|---|---|---|---|---|
| 528 x 461, 8 ports | software | 2.0 to 2.9 ms | 1.4 to 1.9 ms | 1.7 to 2.2 ms | 12.5 to 16.0 ms |
| | hardware | 0.19 to 0.22 ms | 0.63 to 0.65 ms | 1.3 to 1.4 ms | 12.2 to 12.9 ms |
| 40 ports, 1920 x 1080 window | software | 3.2 to 3.6 ms | 3.3 to 3.9 ms | 0.47 to 0.55 ms | 17.4 to 20.3 ms |
| | hardware | 0.25 ms | 0.77 to 0.83 ms | 0.61 to 0.63 ms | 16.3 to 17.5 ms |

The first frame after start is shown 220 to 250 ms later with the GPU (590 to 620 ms against 364 to 420 ms): the
graphics device and the swap chain are made then.

The first measurements of this design had the GPU at 17 to 21 ms for a repaint and 51 to 61 ms for a resize step.
`12-render-profiling.md` explains them (the wait for the display, the flip swap chain, a viewport repaint per frame,
textures made again on every move, and GPU timing slowing the frames it timed) and what was changed.

## What was not verified

Only this one machine and GPU: Windows 11, NVIDIA RTX 4090, Direct3D 11. Not run: macOS (Metal), Linux (OpenGL or
Vulkan), AMD or Intel GPUs, other Direct3D or Vulkan backends, a remote desktop session, and a virtual machine without a
GPU (the automatic fallback is covered by unit tests of the rule, not by running on such a machine).
