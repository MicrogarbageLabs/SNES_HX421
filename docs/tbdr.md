# TBDR — tile-based deferred renderer, design

Untextured 4bpp polygon rendering into SNES CHR, with a z buffer, solid-tile compression, and
composite layers. Target is 240x200 (30x25 = **750 tiles**).

## The structural insight: the TBDR tile IS the SNES tile

Bin at **8x8**, the SNES CHR tile size. Then a rasterized tile is not converted into output — it
*is* the output, one finished 4bpp CHR tile. No blit, no repacking, no framebuffer.

That makes the per-tile working set trivial:

```
colour   64 px x 4bpp   =  32 B     (exactly one SNES 4bpp tile)
z        64 px x 16-bit = 128 B
                          -----
                          160 B, or 320 B double-buffered
                          -> under ONE M9K block
```

Compare that to the 5-block estimate in the hardware budget: binning at the SNES tile size makes
the tile buffer almost free, and leaves the z buffer at full 16-bit precision rather than something
squeezed.

## THE CONSTRAINT IS CHR→VRAM DMA, NOT FILL RATE

Fill rate is comfortable and not worth optimising:

```
750 tiles x 64 px x ~3 overdraw = ~144K pixel tests/frame
at 1 px/cycle, 40 MHz            = 3.6 ms   -> ~11% of a 33 ms frame
```

The wall is getting finished tiles into VRAM:

```
vblank DMA budget        ~6.2 KB per 60 Hz frame
full screen, all unique  750 x 32 B = 24 KB  -> 4 vblanks -> 15 fps
```

**So an all-unique-tile 3D screen is a 15 fps proposition.** Everything below is about beating that
number, and it is why "lower frame rate, tile-reduced" was the right instinct.

## Solid-tile compression — the lever that matters

After rasterising, test whether all 64 pixels share one colour index. If so, point the tilemap at a
**pre-uploaded solid tile** (16 of them, one per index, resident) and upload no CHR at all. Blank
sky, flat ground, and untextured faces larger than 8 px all collapse to a tilemap entry.

```
0%  solid  750 unique  24.0 KB   4 vblanks   15 fps
50% solid  375 unique  12.0 KB   2 vblanks   30 fps
67% solid  250 unique   8.0 KB   2 vblanks   30 fps
75% solid  188 unique   6.0 KB   1 vblank    60 fps
```

Frame rate becomes **scene-dependent rather than fixed** — which suits a rail shooter, where the
sky is most of the screen, far better than a fixed budget would.

Worth building the counter early: emit `unique_tiles` per frame from the start, so the fps a given
scene can sustain is measured rather than guessed. The same discipline that made the burst-V
telemetry pay.

## VRAM

```
CHR, one frame of unique tiles   750 x 32 B  = 24 KB    (fewer with solid tiles)
double-buffered                              = 48 KB    <- does not fit alongside anything
```

So the **CHR-base overlap** applies here exactly as it does for FMV: place the back buffer one
4096-word granule early, reclaim 8 KB, and write the contested tiles last. See
`docs/fmv-engine.md`; the mechanism is identical and already proven.

With solid-tile compression the working set shrinks further, since only unique tiles need slots at
all — a tile allocator per frame, not a fixed 750-slot reservation.

## Z buffer

16-bit per pixel, 64 px per tile, resident only for the tile being rasterised. Because it is
per-tile rather than per-screen it costs 128 B instead of 96 KB, which is the whole reason TBDR
suits this hardware.

It also buys the thing that makes 3D-plus-sprites work: **sprites can be depth-tested against the
scene**. A 3D object rendered into OBJ CHR can be occluded correctly by BG geometry, which a
painter's-algorithm renderer cannot do.

## Composite layers

- **BG1, 4bpp** — the main polygon render, unique + solid tiles.
- **BG2** — a second layer at a *lower* update rate for background/parallax, so its CHR cost is
  amortised across several frames. Cheap colour headroom.
- **Sprites** — overlay, and additionally a *render target*: drawing 3D objects into OBJ CHR gives
  them their own palettes (OBJ palettes are CGRAM 128-255, independent of BG), which is extra
  colour the BG layers cannot reach.

Later: **60-colour blending of a 2bpp and a 4bpp layer** for a full-screen 240x200 framebuffer —
the technique already explored for FMV. Filed as the endpoint, not the starting point.

## Build order

1. **Rasteriser core in C** — bin, edge-test, z-test, into a 64-px tile. Portable and integer-only,
   like `hx421_metatile.c`, so it doubles as the RTL spec and can be diffed in simulation.
2. **Solid-tile detection + counter** — the compression and the measurement together, so the fps
   claim above is verified before anything depends on it.
3. **Tile allocator + CHR/tilemap emission** into the existing staging path.
4. **Demo in bsnes** — spinning objects over the existing map layers.
5. **Sprite render target**, then the dual-layer colour work.

Steps 1-2 answer the question the whole design rests on: what fraction of a real scene is solid.
Everything after that is plumbing we have already built once for the tilemap accelerator.
