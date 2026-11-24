# Changelog

All notable changes to Midori are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.6] - 2026-11-24

### Added

- Canvas
  - *Off-theme:* "Tile LOD bias" slider in Settings > Display (`tileLodBias` in `preferences.json`, from -3.00 to
    +3.00): shifts the tile mip level chosen for a view's zoom. Negative values keep finer tiles when zoomed out;
    positive values pick coarser tiles sooner.

## [0.2.5] - 2026-11-16

### Fixed

- Input
  - Switching away from a window with Alt+Tab left Alt held in Midori, which kept the eyedropper active. Losing focus
    now clears held modifiers and other temporary input and UI state.

## [0.2.4] - 2026-11-11

### Fixed

- Development
  - The in-app benchmark painted nothing and never finished when the current tool used another brush than the built-in
    one: it waited for the built-in brush, which only compiles once a tool picks it. It now asks for it, times from its
    first stroke, and logs why it stops whenever it does.

## [0.2.3] - 2026-11-11

### Added

- Development
  - Tests: `midori_tests`, run by `ctest --preset <preset>`, a small runner of Midori's own with no dependency. They
    cover tile keys and their order, the atlas's Morton cells, curves, mdsl (the built-in brush, determinism, errors),
    key bindings and sequences, fuzzy search, viewport math and colour conversions.
  - CPU benchmark: `midori_bench`, the fastest and median time per call of mdsl translation and stamp(), QOI tile
    encoding and decoding, the F1 search, curve baking, key presses and OkLab conversions.
  - In-app benchmark (Debug > Benchmark, `debug.benchmark`): paints a generated 4-layer canvas, sweeps the view across
    it, saves, opens and exports it in a tab of its own, then shows the timings in the statistics window and appends
    them to `benchmarks.csv` beside `preferences.json`.

### Changed

- Development
  - The core builds as a static library, `midori_core`, which the app, the tests and the benchmark link.

## [0.2.2] - 2026-11-11

### Changed

- Portability
  - Canvas files and tile caches open through one helper: libmdbx's wide-path calls on Windows, which keep paths
    with accented characters working there, and its UTF-8 calls elsewhere. The core no longer calls Windows-only
    libmdbx functions, a first step toward the iPad port.

## [0.2.1] - 2026-11-11

### Added

- Files
  - *Off-theme:* "Include the background colour" check box under the file type when exporting, on by default and
    remembered until Midori closes. Checked, the image is drawn over the canvas's background colour as the view shows
    it, a selection's in its shape; unchecked, it keeps its transparency.

### Changed

- Files
  - The open and save dialogs use the system's modern file dialog (IFileDialog) instead of the older one.

## [0.2.0] - 2026-11-11

Groundwork. This first version carries one off-theme change, approved because the proof of concept could not get an
image out.

### Added

- Files
  - *Off-theme:* Export to PNG (File > Export..., Ctrl+Shift+E, `canvas.export`): what the canvas shows, without the
    background colour so transparency is kept. The selection exports its bounds through its coverage; without one,
    everything drawn, cropped to the visible pixels. Up to 16384 pixels a side.

## [0.1.0] - 2026-11-11

The proof of concept: an infinite, tiled raster canvas on Windows (Direct3D 12).

### Added

- Canvas
  - Infinite canvas made of 256x256 tiles, streamed in and out of a GPU tile atlas as the view moves.
  - Navigation: pan, zoom, rotate (snapping to 15°), and flip horizontally or vertically, held on Space.
  - Navigation with the mouse alone: right drag pans, the wheel zooms around the pointer, and the wheel during a right
    drag rotates around it, 15° a notch.
  - Zoom steps through texel-friendly levels (=, -, numpad + and -), and Ctrl+1 for the actual size.
  - Navigation history: Space+Z goes back to the previous view and Space+Shift+Z forward again. A quick run of wheel
    notches or zoom keys is one step.
  - Transforms snap to whole pixels, so an unrotated canvas stays sharp at whole zoom levels.
  - Coarser tile levels used when zoomed out, rebuilt after an edit.
  - Display filters: nearest, bilinear, bicubic, Mitchell, Lanczos-2 (default) and Lanczos-3.
  - Background colour in the View panel.
- Files
  - The `.mori` canvas file, stored with libmdbx, with tiles compressed as QOI.
  - New, Open, Save and Save As, asking before unsaved changes are lost.
  - Recent files, and a setting to reopen the files that were open at the last exit, each with its view (position,
    zoom, rotation and flips).
  - Files given on the command line each open in their own window.
  - Tiles not shown are moved to a temporary disk cache, so canvases can be larger than video memory.
  - Autosave: every 3 minutes of activity, unsaved canvases are written to a recovery folder, never over their files.
    After a crash, the next launch offers to recover them as unsaved canvases.
- Painting
  - Brush strokes with pen pressure and tilt through Wintab, falling back to the mouse.
  - Flow and opacity that behave like Photoshop's: a stroke never goes past its opacity, even over itself.
  - Blend modes: Normal, Multiply, Add, Screen, Normal (OkLab), Min, Max and Erase.
  - Erasing is a blend mode, so one brush paints and erases. Its colour sets how much of each channel is removed.
  - Box and lasso shapes filled with the brush.
  - Brush outline under the pointer.
  - Brush size, opacity and hardness changed by dragging (F, Shift+F, Ctrl+F).
  - Brush size in steps ([ and ]) and opacity by tenths from the number keys (1 to 9, 0 for full).
- Brushes
  - mdsl, Midori's brush language: a brush is a `.mdsl` file with a `stamp()` run on the CPU and a `paint()` run on
    the GPU, compiled when Midori runs and read again whenever the file changes.
  - Brush params with sliders, curves and textures declared in the brush file.
  - Each param can follow pressure, tilt, speed or randomness through a curve.
  - `round.mdsl`, the built-in round brush, which also documents the language.
- Tools
  - A tool library (`tools.json`) and a toolbar arranged from it (`workspace.json`). Each tool is a brush with a
    shape, a target (layer, selection or tags) and a blend mode or selection operation.
  - Toolbar slots holding several tools, cycled by pressing the slot's key again.
  - Duplicating, renaming and deleting tools.
  - Eyedropper tool (I): dragging over the canvas picks a colour into the current colour slot, either what the canvas
    shows (layers over the background) or the active layer's own colour. Any tool can become one by targeting the
    colour.
  - Holding Alt with a brush in paint mode picks colours like the eyedropper (`tool.eyedropper`).
- Colours
  - Colour panel with several colour slots (X and Shift+X cycle through them) and recently painted colours.
- Layers
  - Layers and folders, with opacity, blend mode, visibility and names.
  - New, delete, move, merge down and flatten a folder.
  - Selecting several layers at once.
  - Locking layers, from their lock box or `layer.toggleLocked`: a locked layer's pixels take no painting, erasing,
    cutting or transforming, and a locked folder locks everything inside it. Undoable.
- Selection
  - Rectangle and lasso selections, combining with the previous one by replace, add, subtract, intersect or exclude
    (Shift, Alt and Shift+Alt while dragging).
  - Strokes paint only inside the selection.
  - Selection outline drawn as dashes.
  - Delete clears the selected pixels; Ctrl+D selects nothing.
- Transform
  - Ctrl+T moves, scales and rotates the selected pixels. Alt drags a corner or an edge alone, for a perspective
    distortion. Enter applies the transform, Escape cancels it.
  - Holding Ctrl with any tool shows the selection's transform box, ready to drag.
- Clipboard
  - Copy, cut and paste images, shared with other applications as PNG and bitmap.
  - Pasting puts the image in a new layer, ready to transform. A Midori copy pastes back where it came from.
- Tags
  - Tags name areas of the canvas to organize it and find them again. The Tag tool adds or removes areas from the
    checked tags.
  - Tags panel to select, rename and colour tags, and jump between their areas (Left and Right).
  - Deleting a tag from a right click on it, or from the F1 search (`tag.delete`), after confirming.
- Windows, tabs and split views
  - Several canvases in one window as tabs, and one canvas shown again in another tab, pane or window from its own
    position. New and Open add a tab.
  - Split a pane right (Ctrl+K, Right) or down (Ctrl+K, Down), resize the split by dragging the handle between them,
    and focus a pane by clicking in it.
  - New window showing the current canvas (Ctrl+K, N), duplicate a tab (Ctrl+K, D), close a tab (Ctrl+W or middle
    click), and go through a pane's tabs (Ctrl+Tab and Ctrl+Shift+Tab).
  - A pane shows its tab strip once it holds two tabs.
  - Reopening the last files brings each window's files back as its tabs.
- Undo and redo
  - For strokes, transforms, selections and layer changes (Ctrl+Z, Ctrl+Shift+Z or Ctrl+Y).
- Interface
  - Panels for the view, tool, toolbar, tools, layers, tags, colours, settings and help.
  - Every command is an action. F1 searches them, and also layers, tools, tags, recent files and settings.
  - Key bindings can be changed in `keybinds.json`, including sequences of keys and keys that act while held.
  - A popup lists the keys that can follow a started key sequence.
  - English, French and Japanese, following the system language unless one is chosen in the settings.
  - Text drawn with the system's fonts through FreeType.
  - Fullscreen (F11) and hiding the interface (Tab).
  - Preferences kept between runs in `preferences.json`.
- Debugging
  - Tile atlas viewer, tile cell outlines on the canvas, statistics on memory, jobs, caches and queues, and Tracy
    profiling.

### Changed

- Brush params can follow several inputs at once (pressure, tilt X, tilt Y, speed and random), each through its own
  curve. The value painted is the param's value times every input's curve. Older `tools.json` files, with one input per
  param, still load.

### Fixed

- Painting and drawing far from the canvas origin no longer loses precision.
- No more seams between tiles while zooming.
- Tiles being edited now match the zoomed-out levels of the tiles around them.
- The scroll bar of the F1 search now scrolls.
