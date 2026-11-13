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
`QT_WIDGETS_RHI_BACKEND=d3d12|vulkan|opengl|d3d11|metal` changes it for diagnosis.

## Profiling

All behind the existing run-time switch; with the profiler dormant the new code adds one relaxed load per zone.

| Zone, plot or message | Meaning |
|---|---|
| `gui.render.frame` | `GpuCanvas::render`: the CPU time of one GPU frame |
| `gui.render.draw`, `.dots`, `.items` | recording the draw calls, the grid, the items |
| `gui.render.upload` | an item drawn into a new texture (the CPU part that remains) |
| `gui.render.sync` | invalidating textures from the scene's changed regions, dropping those of deleted items |
| `gui.render.view` | the view's own paint event (a request for the next GPU frame) |
| `gui.gpu.frame` | GPU time of a frame, from QRhi (`lastCompletedGpuTime`), in a Tracy GPU context named `gui.canvas` |
| `gui.render.item_textures`, `gui.render.texture_bytes` | plots |
| `gui.render_mode` | message at start-up and at a fallback |

QRhi gives one duration per frame, not a start time, so the `gui.gpu.frame` zone ends when the duration is read and
starts that long before: its length is the GPU's, its place on the timeline is within a frame or two. It uses Tracy's C
API for custom GPU contexts, so the same code serves every graphics API. Qt only measures GPU time when it is asked to
at device creation, so Studio sets `QSG_RHI_PROFILE=1` when the profiler is already recording at start-up. Started later
from Settings, the CPU zones record and the GPU zone needs a restart. Nothing is sent anywhere: the listener is the
existing one, on this computer only.

## Measurements

`vss_gui_render_bench` (Release with Tracy, Windows 11, NVIDIA GeForce RTX 4090, Direct3D 11, Qt 6.12). "Canvas
drawing" is the CPU time the canvas spent in the frame. "Step" is the time until the event loop has handled the change,
which includes composing and presenting the window.

| Canvas | | Canvas drawing | Repaint step | Resize step, mean / slowest |
|---|---|---|---|---|
| 528 x 461, 8 ports | software | 2.2 ms | 3.0 ms | 18 / 36 ms |
| | hardware | 0.05 ms | 17 to 21 ms | 51 to 61 / 123 to 510 ms |
| 1644 x 807, 40 ports | software | 3.4 ms | 5.1 ms | 27 / 54 ms |
| | hardware | 0.12 ms | 21.6 ms | 71 to 77 / 149 to 167 ms |

In a Debug build the software canvas drawing was 14 ms per repaint of the small canvas and 0.25 ms in hardware.

What the numbers say, plainly: the GPU takes the canvas's CPU drawing from milliseconds to about a tenth of a
millisecond, and the GPU spends 0.03 to 0.3 ms on it. But a window that holds a GPU widget is composed through the
graphics API, and the present waits for the display, so a repaint step is about one display frame and a resize is slower
than on the plain path. For canvases of this size the software path is the faster one to resize. The GPU path pays when
the canvas, and the CPU time to draw it, is much larger than this, and it frees the processor from drawing it.

## What was not verified

Only this one machine and GPU: Windows 11, NVIDIA RTX 4090, Direct3D 11. Not run: macOS (Metal), Linux (OpenGL or
Vulkan), AMD or Intel GPUs, other Direct3D or Vulkan backends, a remote desktop session, and a virtual machine without a
GPU (the automatic fallback is covered by unit tests of the rule, not by running on such a machine).
