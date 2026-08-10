/* AUD2GO -- Sound Blaster Audigy 2 ZS Notebook (SB0530) enabler for DOS
 *
 * The BIOS leaves CardBus sockets completely unconfigured in DOS: no socket
 * register window, no bus numbers in use, no power. AUD2GO powers the socket,
 * brings the card out of reset and assigns it PCI resources, so a DOS driver
 * can find and drive it.
 *
 * It deliberately does NOT touch the audio silicon: the CA0108 needs a
 * wake-up sequence before its I/O registers respond, and that belongs to
 * whatever drives the chip -- VSBPCMA performs it at start-up, and CBINIT
 * (shipped alongside the driver) does it standalone for driverless probing.
 *
 * Copyright (c) 2026 zikolas. MIT licence -- see LICENSE.
 *
 * Build on DOS with OpenWatcom:  BLD.BAT      (16-bit real mode, C89)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define VERSION "1.1"

/* ---- Yenta CardBus socket registers, memory mapped at the socket base ---- */
#define CB_SOCKET_EVENT     0x00
#define CB_SOCKET_STATE     0x08
#define CB_SOCKET_CONTROL   0x10
#define CB_SOCKET_POWER     0x20

#define CB_CDETECT1         0x00000002UL
#define CB_CDETECT2         0x00000004UL
#define CB_PWRCYCLE         0x00000008UL
#define CB_16BITCARD        0x00000010UL
#define CB_CBCARD           0x00000020UL
#define CB_NOTACARD         0x00000080UL
#define CB_BADVCCREQ        0x00000200UL
#define CB_5VCARD           0x00000400UL
#define CB_3VCARD           0x00000800UL

#define CB_SC_VCC_5V        0x20
#define CB_SC_VCC_3V        0x30
#define CB_SC_VPP_5V        0x02
#define CB_SC_VPP_3V        0x03

/* ---- CardBus bridge config space (PCI header type 2) ---- */
#define CFG_CMD             0x04
#define CFG_SOCKBASE        0x10
#define CFG_CBBUS           0x19
#define CFG_MEM0BASE        0x1C
#define CFG_MEM0LIM         0x20
#define CFG_MEM1BASE        0x24
#define CFG_MEM1LIM         0x28
#define CFG_IO0BASE         0x2C
#define CFG_IO0LIM          0x30
#define CFG_IO1BASE         0x34
#define CFG_IO1LIM          0x38
#define CFG_INTLINE         0x3C
#define CFG_BRIDGECTL       0x3E
#define CFG_LEGACYBASE      0x44
#define BCTL_CRST           0x0040

/* ---- the card we know about ---- */
#define CREATIVE_VENDOR     0x1102
#define CA0108_DEVICE       0x0008
#define ZSNB_SUBSYS         0x20011102UL


static unsigned g_sockseg = 0xD000;   /* real-mode segment for socket regs */
static unsigned g_iobase  = 0x1400;
static int      g_verbose = 0;
static int      g_force   = 0;

struct bridge_s { unsigned bus, dev, fn; };
static struct bridge_s g_br[8];
static int g_nbr = 0;

/* ------------------------------------------------------------ 32-bit I/O */
/* Raw opcodes so this stays ordinary 16-bit real-mode code.
 *   66 ED         in  eax, dx      66 8B D0   mov edx, eax
 *   66 C1 EA 10   shr edx, 16      -> long returned in DX:AX               */
unsigned long ind(unsigned port);
#pragma aux ind =           \
    0x66 0xED  0x66 0x8B 0xD0  0x66 0xC1 0xEA 0x10  \
    parm [dx] value [dx ax] modify [dx ax];

/*   66 0F B7 C0  movzx eax, ax    66 C1 E2 10  shl edx,16
 *   66 0B C2     or  eax, edx     8B D1        mov dx, cx
 *   66 EF        out dx, eax                                              */
void outd(unsigned port, unsigned long val);
#pragma aux outd =          \
    0x66 0x0F 0xB7 0xC0  0x66 0xC1 0xE2 0x10  0x66 0x0B 0xC2  \
    0x8B 0xD1  0x66 0xEF  \
    parm [cx] [dx ax] modify [dx ax];

/* ------------------------------------------------------------- PCI BIOS */

static int pci_present(unsigned char *lastbus)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x01;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    *lastbus = r.h.cl;
    return 1;
}

static unsigned char cfg_rb(unsigned bus, unsigned devfn, unsigned reg)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x08;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0xFF;
    return r.h.cl;
}

static unsigned cfg_rw(unsigned bus, unsigned devfn, unsigned reg)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x09;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0xFFFF;
    return r.x.cx;
}

static unsigned long cfg_rd(unsigned bus, unsigned devfn, unsigned reg)
{
    unsigned long hi = cfg_rw(bus, devfn, reg + 2);
    return (hi << 16) | cfg_rw(bus, devfn, reg);
}

static void cfg_wb(unsigned bus, unsigned devfn, unsigned reg, unsigned char v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x0B;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.h.cl = v;
    r.x.di = reg;
    int86(0x1A, &r, &r);
}

static void cfg_ww(unsigned bus, unsigned devfn, unsigned reg, unsigned v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x0C;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.cx = v;
    r.x.di = reg;
    int86(0x1A, &r, &r);
}

static void cfg_wd(unsigned bus, unsigned devfn, unsigned reg, unsigned long v)
{
    cfg_ww(bus, devfn, reg, (unsigned)(v & 0xFFFFUL));
    cfg_ww(bus, devfn, reg + 2, (unsigned)(v >> 16));
}

/* -------------------------------------------------- socket register MMIO */

static unsigned long sock_rd(unsigned reg)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(g_sockseg, reg);
    unsigned long v = 0;
    int i;

    for (i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void sock_wr(unsigned reg, unsigned long v)
{
    unsigned char __far *p = (unsigned char __far *)MK_FP(g_sockseg, reg);
    int i;

    for (i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v & 0xFF);
        v >>= 8;
    }
}

static void tick_delay(int ticks)
{
    unsigned long volatile __far *bios;
    unsigned long start;

    bios = (unsigned long volatile __far *)MK_FP(0x0040, 0x006C);
    start = *bios;
    while ((*bios - start) < (unsigned long)ticks)
        /* spin */;
}

/* ------------------------------------------------------------ bridge ops */

/* Disable every window we are not using. A PCI bridge window is disabled by
 * making base greater than limit; left at 0/0 the bridge claims low memory
 * and I/O ports 0-3 (the DMA controller) the moment decoding is enabled.
 * The legacy ExCA base defaults to 0x00000001 -- enabled, at port 0. */
static void disable_windows(unsigned bus, unsigned devfn)
{
    cfg_wd(bus, devfn, CFG_MEM0BASE, 0xFFFFFFFFUL);
    cfg_wd(bus, devfn, CFG_MEM0LIM,  0x00000000UL);
    cfg_wd(bus, devfn, CFG_MEM1BASE, 0xFFFFFFFFUL);
    cfg_wd(bus, devfn, CFG_MEM1LIM,  0x00000000UL);
    cfg_wd(bus, devfn, CFG_IO0BASE,  0xFFFFFFFFUL);
    cfg_wd(bus, devfn, CFG_IO0LIM,   0x00000000UL);
    cfg_wd(bus, devfn, CFG_IO1BASE,  0xFFFFFFFFUL);
    cfg_wd(bus, devfn, CFG_IO1LIM,   0x00000000UL);
    cfg_wd(bus, devfn, CFG_LEGACYBASE, 0x00000000UL);
}

static void find_bridges(unsigned char lastbus)
{
    unsigned bus, dev, fn, devfn, ven;
    unsigned char hdr;

    g_nbr = 0;
    for (bus = 0; bus <= lastbus && g_nbr < 8; bus++) {
        for (dev = 0; dev < 32 && g_nbr < 8; dev++) {
            hdr = cfg_rb(bus, dev << 3, 0x0E);
            for (fn = 0; fn < 8; fn++) {
                if (fn && !(hdr & 0x80))
                    break;
                devfn = (dev << 3) | fn;
                ven = cfg_rw(bus, devfn, 0x00);
                if (ven == 0xFFFF || ven == 0x0000)
                    continue;
                if (cfg_rb(bus, devfn, 0x0B) == 0x06 &&
                    cfg_rb(bus, devfn, 0x0A) == 0x07) {
                    g_br[g_nbr].bus = bus;
                    g_br[g_nbr].dev = dev;
                    g_br[g_nbr].fn  = fn;
                    if (++g_nbr >= 8)
                        break;
                }
            }
        }
    }
}

static unsigned bdevfn(int i)
{
    return (g_br[i].dev << 3) | g_br[i].fn;
}

/* map a bridge's socket registers so we can read the card-detect state */
static unsigned long socket_state(int i)
{
    disable_windows(g_br[i].bus, bdevfn(i));
    cfg_wd(g_br[i].bus, bdevfn(i), CFG_SOCKBASE, (unsigned long)g_sockseg << 4);
    cfg_ww(g_br[i].bus, bdevfn(i), CFG_CMD, 0x0002);   /* memory decode only */
    return sock_rd(CB_SOCKET_STATE);
}

static void socket_unmap(int i)
{
    cfg_ww(g_br[i].bus, bdevfn(i), CFG_CMD, 0x0000);
    cfg_wd(g_br[i].bus, bdevfn(i), CFG_SOCKBASE, 0x00000000UL);
}

/* Both card-detect pins low means a card is fully seated. With an empty
 * socket every other status bit floats and must not be believed. */
static int card_seated(unsigned long st)
{
    return (st & (CB_CDETECT1 | CB_CDETECT2)) == 0;
}

/* ...but detect pins alone are not enough: an empty RL5C475 socket reads both
 * of them low, so also insist the bridge reports an actual card type. */
static int card_present(unsigned long st)
{
    return card_seated(st) && (st & (CB_CBCARD | CB_16BITCARD)) != 0;
}

/* Is a card already up and running? The EMU10K is a continuous bus master --
 * its voice engine DMAs even when idle -- so tearing the bridge down to
 * reconfigure it (which clears bus-master enable) while a driver is using the
 * card can wedge the PCI bus outright. Detect that and refuse. */
static int already_configured(int *pbr, unsigned *pdev, unsigned *pcbbus)
{
    int i;
    unsigned cbbus, dev, ven;

    for (i = 0; i < g_nbr; i++) {
        if ((cfg_rw(g_br[i].bus, bdevfn(i), CFG_CMD) & 0x0007) != 0x0007)
            continue;                       /* bridge not fully enabled */
        cbbus = cfg_rb(g_br[i].bus, bdevfn(i), CFG_CBBUS);
        for (dev = 0; dev < 32; dev++) {
            ven = cfg_rw(cbbus, dev << 3, 0x00);
            if (ven == 0xFFFF || ven == 0x0000)
                continue;
            if (cfg_rw(cbbus, dev << 3, 0x04) & 0x0001) {   /* card decoding */
                *pbr = i; *pdev = dev; *pcbbus = cbbus;
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    printf("AUD2GO %s - Sound Blaster Audigy 2 ZS Notebook enabler for DOS\n\n",
           VERSION);
    printf("  AUD2GO           enable the card\n");
    printf("  AUD2GO /SCAN     report what is in the sockets, change nothing\n");
    printf("  AUD2GO /OFF      power the socket back down\n");
    printf("  AUD2GO /V        verbose (show every step)\n\n");
    printf("  /S=n     use socket n (1-based) instead of searching\n");
    printf("  /IO=nnn  I/O base to assign          (default %04X)\n", g_iobase);
    printf("  /SB=nnn  segment for socket registers (default %04X)\n", g_sockseg);
    printf("  /FORCE   reconfigure even if a card is already enabled\n");
}

int main(int argc, char **argv)
{
    unsigned char lastbus, intline;
    int i, target = -1, scan_only = 0, power_off = 0;
    unsigned long st, subsys;
    unsigned devfn, cbbus, ven, did, dev, sz, iolim;
    int known;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            continue;
        if (!strnicmp(argv[i] + 1, "IO=", 3))
            g_iobase = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else if (!strnicmp(argv[i] + 1, "SB=", 3))
            g_sockseg = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else if (!strnicmp(argv[i] + 1, "S=", 2))
            target = atoi(argv[i] + 3) - 1;      /* 1-based for humans */
        else if (!stricmp(argv[i] + 1, "SCAN"))
            scan_only = 1;
        else if (!stricmp(argv[i] + 1, "OFF"))
            power_off = 1;
        else if (!stricmp(argv[i] + 1, "FORCE"))
            g_force = 1;
        else if (!stricmp(argv[i] + 1, "V"))
            g_verbose = 1;
        else {
            usage();
            return 1;
        }
    }

    if (!pci_present(&lastbus)) {
        printf("AUD2GO: no PCI BIOS - this machine has no CardBus sockets.\n");
        return 1;
    }

    find_bridges(lastbus);
    if (!g_nbr) {
        printf("AUD2GO: no CardBus sockets found.\n");
        return 1;
    }

    /* Never pull a running card apart underneath a driver -- see
     * already_configured(). */
    if (!scan_only && !power_off && !g_force) {
        int b;
        unsigned d, cb;
        if (already_configured(&b, &d, &cb)) {
            printf("AUD2GO: socket %d is already enabled at I/O %04lX.\n",
                   b + 1, cfg_rd(cb, d << 3, 0x10) & 0xFFFCUL);
            printf("        A driver may be using it, so nothing was changed.\n");
            printf("        Reboot, or use /FORCE to reconfigure anyway.\n");
            return 0;
        }
    }
    if (g_verbose)
        printf("AUD2GO %s: %d CardBus socket(s), I/O base %04X, "
               "socket regs at %04X:0000\n",
               VERSION, g_nbr, g_iobase, g_sockseg);

    /* look at each socket; keep the first holding a CardBus card */
    for (i = 0; i < g_nbr; i++) {
        st = socket_state(i);
        if (g_verbose)
            printf("  socket %d (%02X:%02X.%X) state %08lX\n",
                   i + 1, g_br[i].bus, g_br[i].dev, g_br[i].fn, st);
        if (!card_present(st)) {
            if (scan_only)
                printf("  Socket %d: empty\n", i + 1);
            socket_unmap(i);
            continue;
        }
        if (scan_only)
            printf("  Socket %d: %s card%s\n", i + 1,
                   (st & CB_CBCARD) ? "CardBus" : "16-bit PC",
                   (st & CB_PWRCYCLE) ? ", powered" : "");
        if (target < 0 && (st & CB_CBCARD) && !(st & CB_NOTACARD))
            target = i;
        else if (target != i)
            socket_unmap(i);
    }

    if (scan_only) {
        if (target >= 0)
            socket_unmap(target);
        return 0;
    }
    if (target < 0 || target >= g_nbr) {
        printf("AUD2GO: no CardBus card found.\n");
        return 1;
    }

    devfn = bdevfn(target);
    st = socket_state(target);

    if (power_off) {
        sock_wr(CB_SOCKET_CONTROL, 0x00);
        tick_delay(6);
        socket_unmap(target);
        printf("AUD2GO: socket %d powered down.\n", target + 1);
        return 0;
    }

    /* ---- power the socket ---- */
    cfg_ww(g_br[target].bus, devfn, CFG_BRIDGECTL,
           cfg_rw(g_br[target].bus, devfn, CFG_BRIDGECTL) | BCTL_CRST);
    if (st & CB_3VCARD)
        sock_wr(CB_SOCKET_CONTROL, CB_SC_VCC_3V | CB_SC_VPP_3V);
    else if (st & CB_5VCARD)
        sock_wr(CB_SOCKET_CONTROL, CB_SC_VCC_5V | CB_SC_VPP_5V);
    else {
        printf("AUD2GO: the card in socket %d reports no supported voltage.\n",
               target + 1);
        socket_unmap(target);
        return 1;
    }
    if (g_verbose)
        printf("  applying Vcc %s\n", (st & CB_3VCARD) ? "3.3V" : "5V");
    tick_delay(8);

    st = sock_rd(CB_SOCKET_STATE);
    if (!(st & CB_PWRCYCLE)) {
        printf("AUD2GO: socket %d would not power up%s.\n", target + 1,
               (st & CB_BADVCCREQ) ? " (bad Vcc request)" : "");
        socket_unmap(target);
        return 1;
    }
    cfg_ww(g_br[target].bus, devfn, CFG_BRIDGECTL,
           cfg_rw(g_br[target].bus, devfn, CFG_BRIDGECTL) & ~BCTL_CRST);
    tick_delay(4);

    /* ---- find the card on the secondary bus ---- */
    cbbus   = cfg_rb(g_br[target].bus, devfn, CFG_CBBUS);
    intline = cfg_rb(g_br[target].bus, devfn, CFG_INTLINE);
    dev = 0xFFFF;
    for (i = 0; i < 32; i++) {
        ven = cfg_rw(cbbus, i << 3, 0x00);
        if (ven != 0xFFFF && ven != 0x0000) {
            dev = i;
            break;
        }
    }
    if (dev == 0xFFFF) {
        printf("AUD2GO: the card in socket %d did not respond.\n", target + 1);
        socket_unmap(target);
        return 1;
    }
    ven    = cfg_rw(cbbus, dev << 3, 0x00);
    did    = cfg_rw(cbbus, dev << 3, 0x02);
    subsys = cfg_rd(cbbus, dev << 3, 0x2C);
    known  = (ven == CREATIVE_VENDOR && did == CA0108_DEVICE &&
              subsys == ZSNB_SUBSYS);

    if (g_verbose)
        printf("  bus %02X dev %02X: %04X:%04X subsys %08lX class %02X%02X\n",
               cbbus, dev, ven, did, subsys,
               cfg_rb(cbbus, dev << 3, 0x0B), cfg_rb(cbbus, dev << 3, 0x0A));

    /* Only touch it if it is actually a sound card. */
    if (cfg_rb(cbbus, dev << 3, 0x0B) != 0x04) {
        printf("AUD2GO: socket %d holds %04X:%04X, which is not a sound card.\n",
               target + 1, ven, did);
        printf("        Nothing was changed.\n");
        socket_unmap(target);
        return 1;
    }

    /* ---- assign resources ---- */
    cfg_wd(cbbus, dev << 3, 0x10, 0xFFFFFFFFUL);
    sz = (unsigned)(cfg_rd(cbbus, dev << 3, 0x10) & 0xFFFCUL);
    sz = (unsigned)((~sz + 1) & 0xFFFF);
    if (!sz || sz > 0x1000) {
        printf("AUD2GO: unexpected I/O size %04X on the card.\n", sz);
        socket_unmap(target);
        return 1;
    }
    cfg_wd(cbbus, dev << 3, 0x10, (unsigned long)g_iobase | 1UL);
    cfg_wb(cbbus, dev << 3, 0x3C, intline);
    cfg_wb(cbbus, dev << 3, 0x0D, 0x20);

    iolim = g_iobase + sz - 1;
    cfg_wd(g_br[target].bus, devfn, CFG_IO0BASE, (unsigned long)g_iobase);
    cfg_wd(g_br[target].bus, devfn, CFG_IO0LIM,  (unsigned long)iolim);
    cfg_ww(g_br[target].bus, devfn, CFG_CMD, 0x0007);   /* io+mem+master */
    cfg_ww(cbbus, dev << 3, 0x04, 0x0005);              /* io+master */

    if (known)
        printf("Audigy 2 ZS Notebook [SB0530] in socket %d: enabled at "
               "I/O %04X-%04X, IRQ %d.\n"
               "The audio chip still needs its wake-up: VSBPCMA does it at "
               "start-up,\nor run CBINIT for driverless probing.\n",
               target + 1, g_iobase, iolim, intline);
    else {
        printf("Sound card %04X:%04X in socket %d: enabled at I/O %04X-%04X, "
               "IRQ %d.\n", ven, did, target + 1, g_iobase, iolim, intline);
        printf("This is not an Audigy 2 ZS Notebook, so its chip was not "
               "woken up.\n");
    }
    return 0;
}
