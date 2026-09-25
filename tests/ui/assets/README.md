# UI smoke fixtures

`controls.rml` is an engine-authored correctness fixture, not a proposed game screen.
It exercises public RmlUi controls, text editing, scrolling, rectangular clipping,
alpha, a translated element and an external image.

`checker.png` is an engine-authored 2×2 RGBA PNG. Its texels, in row order, are
`(255,0,0,255)`, `(0,255,0,255)`, `(0,0,255,255)`, `(255,255,255,128)`.
The GPU harness samples their centers after bilinear magnification and verifies
linear-light alpha against the clear/scene background.

`LatoLatin-Regular.ttf` is copied without modification from RmlUi 6.3 at
`ba95ffe8bfb6370efb2cdcca927eaad4710c5413`, `Samples/assets/LatoLatin-Regular.ttf`.
The complete upstream sample-font license file is preserved in `LICENSE.txt`,
including Lato's SIL Open Font License 1.1 and copyright. This fixture does not
select a font for games consuming Anima.
