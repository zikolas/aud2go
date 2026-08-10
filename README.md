# AUD2GO

A DOS enabler for the **Sound Blaster Audigy 2 ZS Notebook (SB0530)** — the
CardBus Audigy — plus the CardBus bring-up it needs to exist at all.

In DOS the BIOS leaves CardBus sockets completely unconfigured: no socket
register window, no resources, and **no power to the card**. Nothing in a plain
DOS install fixes that, so the card may as well not be in the slot. AUD2GO
powers the socket, brings the card out of reset and assigns it PCI resources,
so a DOS driver can find and drive it.

AUD2GO stays out of the audio silicon on purpose: the chip's wake-up sequence
belongs with whatever drives it. The VSBPCMA driver performs it at start-up;
for driverless probing use CBINIT, shipped alongside the driver.

Verified on an IBM ThinkPad 235 (Hitachi Prius Note 210) — OPTi 82C700 chipset,
Ricoh RL5C476 + RL5C475 CardBus bridges, three sockets.

```
C:\> AUD2GO
Audigy 2 ZS Notebook [SB0530] in socket 1: enabled at I/O 1400-143F, IRQ 10.
```

## Usage

```
AUD2GO           enable the card
AUD2GO /SCAN     report what is in the sockets, change nothing
AUD2GO /OFF      power the socket back down
AUD2GO /V        verbose (show every step)

  /S=n     use socket n (1-based) instead of searching
  /IO=nnn  I/O base to assign           (default 1400)
  /SB=nnn  segment for socket registers (default D000)
```

AUD2GO only configures the card. To actually get sound you need a driver — see
[vsbpcmcia](https://github.com/zikolas/vsbpcmcia), whose `CARD=AUDIGY` build
provides Sound Blaster emulation on top of it.

If you use a memory manager, **exclude the segment holding the socket
registers**, e.g. `JEMM386 LOAD X=D000-DFFF`.

## What it actually does

1. Finds the CardBus bridges (PCI class 06:07) and maps each one's Yenta socket
   registers into a real-mode-addressable segment, so 16-bit code can read the
   card-detect state without a DPMI host.
2. Picks the socket with a seated CardBus card — gating on both card-detect
   pins being low, because with an empty socket every other status bit floats.
3. Powers it at the voltage the card asks for (3.3V for this one), holding
   CardBus reset across the power-up, then releases it.
4. Enumerates the secondary bus, sizes BAR0, assigns I/O and the bridge's IRQ,
   opens exactly one bridge I/O window and enables decoding.

### Two things that will bite you

**Unused bridge windows must be explicitly disabled.** A PCI bridge window is
disabled by making base greater than limit. Left at 0/0 — which is how the BIOS
leaves them — the bridge starts claiming low memory and **I/O ports 0x0000-0x0003**
the instant you enable decoding, stealing the DMA controller out from under the
chipset. The legacy ExCA base register is worse: it defaults to `0x00000001`,
i.e. *enabled, at port 0*.

**The CA0108 powers up inert.** After the socket is up, the audio chip still
claims cycles in its BAR without completing a *read* — touching it too early
hard-hangs the machine, no recovery, only a reset. Waking it is the driver's
job, not the enabler's: VSBPCMA does it at start-up, and the driver's
`tools/audigy` directory has a standalone `cbinit` for driverless probing.
AUD2GO stops at a configured socket and never touches the audio silicon.

## Building

A prebuilt `AUD2GO.EXE` is included (16-bit real mode, runs on anything).
To rebuild on the DOS box with OpenWatcom (16-bit real mode, C89):

```
BLD.BAT AUD2GO
```

There is no 32-bit or protected-mode dependency anywhere — the socket registers
are mapped low precisely so plain real-mode code can reach them.

## probes/

Small diagnostic tools written while working this out. Not needed for normal
use, but useful on a new machine or a new card:

| tool | what it does |
| --- | --- |
| `pciscan` | list PCI devices / dump a device's config space |
| `pciw` | read or write any PCI config register |
| `pcifind` | check whether the PCI BIOS can see a hot-enabled card |
| `cbio` | staged read test that isolates *where* a CardBus I/O cycle dies |
| `cbtest` | probe the card's registers at a chosen access width |
| `mputest` | check the Audigy's MPU-401 UARTs answer |

Each logs to a `.LOG` file it closes after every line, so if a probe wedges the
machine the log still says exactly which access did it. That is how the inert
register file was found.

Chip-level tools (`cbinit` wake-up, `fxvol`, `dacvol` and the `audmix` mixer)
ship with the [vsbpcmcia](https://github.com/zikolas/vsbpcmcia) driver in its
`tools/audigy/` directory, alongside the code that drives the chip. AUD2GO is
socket and bridge bring-up from the CardBus and ExCA specifications plus bench
measurement.

## Licence

MIT — see [LICENSE](LICENSE).
