# midori

Infinite rasterized painting app (￣ヘ￣)

## Features

- **Infinite canvas**: 256×256 tiled canvas streamed between GPU memory and disk cache, with pan, zoom, rotation, flips, and navigation history
- **Painting & brushes**: Programmable `.mdsl` brush shaders compiled at runtime, with pen pressure, tilt, speed, curves, textures, flow/opacity caps, and Catmull-Rom stroke smoothing
- **Tools & workspace**: Customizable tool library and toolbar drawers (`Path`, `Rectangle`, and `Lasso` shapes), eyedropper, and color slots
- **Layers & blend modes**: Layers and nested folders with locking, merging, opacity, and 8 blend modes (`Normal`, `Normal (OkLab)`, `Multiply`, `Add`, `Screen`, `Min`, `Max`, `Erase`)
- **Selection & transform**: Freehand lasso and rectangle selections (`Replace`, `Add`, `Subtract`, `Intersect`, `Exclude`), clipboard copy/cut/paste, and affine + perspective transforms
- **Tags**: Color-coded canvas tags to mark and jump between areas of an infinite canvas
- **Multi-view workspace**: Multiple windows, tabs, and split panes viewing the same or different `.mori` canvases at once
- **Files & safety**: Compact QOI-compressed `.mori` files, PNG export (whole canvas or selection, with or without background), automatic crash recovery, and full undo/redo
- **Interface**: Dockable panels, `F1` fuzzy command search, customizable key sequences (`keybinds.json`), and English, French, and Japanese localization
