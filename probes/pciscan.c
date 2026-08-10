/* PCISCAN.C -- PCI bus scanner / config-space dumper via PCI BIOS (INT 1Ah)
 *
 *   PCISCAN            list devices on buses 0..lastbus (+ follow bridges)
 *   PCISCAN /A         exhaustive: probe buses 0..15
 *   PCISCAN /D b d f   hex dump 256 bytes of config space for bus:dev.func
 *
 * 16-bit real mode, OpenWatcom:  wcc -ms pciscan.c  /  wlink sys dos ...
 * C89 only.  All PCI access is via PCI BIOS byte reads (AH=B1h AL=08h) so
 * no 32-bit port I/O is needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

static unsigned char g_lastbus = 0;
static unsigned char g_mech = 0;
static unsigned int  g_ver = 0;

static int pci_present(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1;
    r.h.al = 0x01;
    int86(0x1A, &r, &r);
    if (r.x.cflag)
        return 0;
    if (r.h.ah != 0)
        return 0;
    g_mech = r.h.al;
    g_lastbus = r.h.cl;
    g_ver = r.x.bx;
    return 1;
}

static int pci_rb(unsigned bus, unsigned devfn, unsigned reg, unsigned char *v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1;
    r.h.al = 0x08;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag)
        return 0;
    if (r.h.ah != 0)
        return 0;
    *v = r.h.cl;
    return 1;
}

static unsigned pci_rw(unsigned bus, unsigned devfn, unsigned reg)
{
    unsigned char lo, hi;

    lo = hi = 0xFF;
    pci_rb(bus, devfn, reg, &lo);
    pci_rb(bus, devfn, reg + 1, &hi);
    return ((unsigned)hi << 8) | lo;
}

static unsigned long pci_rd(unsigned bus, unsigned devfn, unsigned reg)
{
    unsigned long lo, hi;

    lo = pci_rw(bus, devfn, reg);
    hi = pci_rw(bus, devfn, reg + 2);
    return (hi << 16) | lo;
}

static char *classname(unsigned char cls, unsigned char sub)
{
    switch (cls) {
    case 0x00: return "legacy";
    case 0x01: return "storage";
    case 0x02: return "network";
    case 0x03: return "display";
    case 0x04:
        if (sub == 0x01) return "AUDIO";
        return "multimedia";
    case 0x05: return "memory";
    case 0x06:
        if (sub == 0x00) return "host-bridge";
        if (sub == 0x01) return "isa-bridge";
        if (sub == 0x04) return "pci-bridge";
        if (sub == 0x07) return "CARDBUS-BR";
        return "bridge";
    case 0x07: return "comm";
    case 0x08: return "sys-periph";
    case 0x09: return "input";
    case 0x0A: return "docking";
    case 0x0B: return "cpu";
    case 0x0C:
        if (sub == 0x03) return "usb";
        return "serial-bus";
    default: return "?";
    }
}

static void dump_cfg(unsigned bus, unsigned dev, unsigned fn)
{
    unsigned devfn, i, j;
    unsigned char b[256];
    unsigned char v;

    devfn = (dev << 3) | fn;
    for (i = 0; i < 256; i++) {
        v = 0xFF;
        pci_rb(bus, devfn, i, &v);
        b[i] = v;
    }
    printf("Config space %02X:%02X.%X\n", bus, dev, fn);
    for (i = 0; i < 256; i += 16) {
        printf("%02X:", i);
        for (j = 0; j < 16; j++)
            printf(" %02X", b[i + j]);
        printf("\n");
    }
}

static void show_bars(unsigned bus, unsigned devfn, int nbars)
{
    int i;
    unsigned long v;

    for (i = 0; i < nbars; i++) {
        v = pci_rd(bus, devfn, 0x10 + i * 4);
        if (v == 0UL || v == 0xFFFFFFFFUL)
            continue;
        if (v & 1UL)
            printf("      BAR%d io  %04lX\n", i, v & 0xFFFCUL);
        else
            printf("      BAR%d mem %08lX\n", i, v & 0xFFFFFFF0UL);
    }
}

static void show_dev(unsigned bus, unsigned dev, unsigned fn)
{
    unsigned devfn;
    unsigned ven, did;
    unsigned char cls, sub, pif, hdr, iline, ipin, cmd;
    unsigned long subsys;

    devfn = (dev << 3) | fn;
    ven = pci_rw(bus, devfn, 0x00);
    did = pci_rw(bus, devfn, 0x02);
    cls = sub = pif = hdr = iline = ipin = cmd = 0;
    pci_rb(bus, devfn, 0x0B, &cls);
    pci_rb(bus, devfn, 0x0A, &sub);
    pci_rb(bus, devfn, 0x09, &pif);
    pci_rb(bus, devfn, 0x0E, &hdr);
    pci_rb(bus, devfn, 0x3C, &iline);
    pci_rb(bus, devfn, 0x3D, &ipin);
    pci_rb(bus, devfn, 0x04, &cmd);

    printf("%02X:%02X.%X %04X:%04X cls%02X%02X %-10s hdr%02X cmd%02X irq%d pin%d\n",
           bus, dev, fn, ven, did, cls, sub, classname(cls, sub),
           hdr, cmd, iline, ipin);

    if ((hdr & 0x7F) == 0x00) {
        subsys = pci_rd(bus, devfn, 0x2C);
        printf("      subsys %08lX\n", subsys);
        show_bars(bus, devfn, 6);
    } else if ((hdr & 0x7F) == 0x01) {
        unsigned char p, s, u;
        p = s = u = 0;
        pci_rb(bus, devfn, 0x18, &p);
        pci_rb(bus, devfn, 0x19, &s);
        pci_rb(bus, devfn, 0x1A, &u);
        printf("      pri%02X sec%02X sub%02X\n", p, s, u);
    } else if ((hdr & 0x7F) == 0x02) {
        unsigned char p, s, u;
        unsigned bctl;
        p = s = u = 0;
        pci_rb(bus, devfn, 0x18, &p);
        pci_rb(bus, devfn, 0x19, &s);
        pci_rb(bus, devfn, 0x1A, &u);
        bctl = pci_rw(bus, devfn, 0x3E);
        subsys = pci_rd(bus, devfn, 0x40);
        printf("      CB pri%02X cbbus%02X sub%02X bctl%04X subsys %08lX\n",
               p, s, u, bctl, subsys);
        printf("      sockbase %08lX\n", pci_rd(bus, devfn, 0x10));
        printf("      mem0 %08lX-%08lX  mem1 %08lX-%08lX\n",
               pci_rd(bus, devfn, 0x1C), pci_rd(bus, devfn, 0x20),
               pci_rd(bus, devfn, 0x24), pci_rd(bus, devfn, 0x28));
        printf("      io0  %08lX-%08lX  io1  %08lX-%08lX\n",
               pci_rd(bus, devfn, 0x2C), pci_rd(bus, devfn, 0x30),
               pci_rd(bus, devfn, 0x34), pci_rd(bus, devfn, 0x38));
    }
}

static void scan_bus(unsigned bus)
{
    unsigned dev, fn, ven;
    unsigned char hdr;

    for (dev = 0; dev < 32; dev++) {
        ven = pci_rw(bus, dev << 3, 0x00);
        if (ven == 0xFFFF || ven == 0x0000)
            continue;
        hdr = 0;
        pci_rb(bus, dev << 3, 0x0E, &hdr);
        show_dev(bus, dev, 0);
        if (hdr & 0x80) {
            for (fn = 1; fn < 8; fn++) {
                ven = pci_rw(bus, (dev << 3) | fn, 0x00);
                if (ven == 0xFFFF || ven == 0x0000)
                    continue;
                show_dev(bus, dev, fn);
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned bus, top;

    if (!pci_present()) {
        printf("PCI BIOS not present (INT 1Ah AH=B1h AL=01h failed)\n");
        return 1;
    }
    printf("PCI BIOS ver %X.%02X  mech=%02X  lastbus=%02X\n",
           (g_ver >> 8) & 0xFF, g_ver & 0xFF, g_mech, g_lastbus);

    if (argc >= 5 && (argv[1][0] == '/' || argv[1][0] == '-') &&
        (argv[1][1] == 'd' || argv[1][1] == 'D')) {
        dump_cfg((unsigned)strtoul(argv[2], NULL, 16),
                 (unsigned)strtoul(argv[3], NULL, 16),
                 (unsigned)strtoul(argv[4], NULL, 16));
        return 0;
    }

    top = g_lastbus;
    if (argc >= 2 && (argv[1][0] == '/' || argv[1][0] == '-') &&
        (argv[1][1] == 'a' || argv[1][1] == 'A'))
        top = 15;

    for (bus = 0; bus <= top; bus++) {
        printf("--- bus %02X ---\n", bus);
        scan_bus(bus);
    }
    return 0;
}
