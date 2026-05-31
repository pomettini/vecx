vecx for Playdate
=================

A C-only Playdate port of the `vecx` Vectrex emulator.

Requirements
------------

* Playdate SDK
* `arm-none-eabi-gcc`
* `pdc`

Build
-----

```sh
make
```

The build creates `vecx.pdx` with both simulator and device binaries.

Install to a USB-mounted Playdate:

```sh
make _push
```

Cartridges
----------

The Vectrex BIOS is packaged as `Source/rom.dat`. If you want to test a local cartridge, add it as `Source/cart.vec` before building. The loader also falls back to `Source/mine_storm.vec` when present. `.vec` files are intentionally ignored by Git.

Notes
-----

See `NOTES.md` for the active porting plan, findings, benchmark format, and known limitations.

Authors
-------

* Valavan Manohararajah - original author
* [John Hawthorn](https://twitter.com/jhawthorn) - desktop port
* [Nikita Zimin](https://twitter.com/nzeemin) - audio

Contributors
------------

* [Simon Rodriguez](https://twitter.com/simonkosua) - SDL2 desktop port
