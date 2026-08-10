/* PCIW.C -- read/write PCI config space via PCI BIOS (INT 1Ah AH=B1h)
 *
 *   PCIW b d f reg            read  dword at reg
 *   PCIW b d f reg val        write dword at reg (as two word writes)
 *   PCIW /B b d f reg [val]   byte
 *   PCIW /W b d f reg [val]   word
 *
 * All numbers hex.  16-bit real mode, C89, OpenWatcom wcc -ms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

static int rb(unsigned bus, unsigned devfn, unsigned reg, unsigned char *v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x08;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    *v = r.h.cl;
    return 1;
}

static int rw(unsigned bus, unsigned devfn, unsigned reg, unsigned *v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x09;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    *v = r.x.cx;
    return 1;
}

static int wb(unsigned bus, unsigned devfn, unsigned reg, unsigned char v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x0B;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.h.cl = v;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    return 1;
}

static int ww(unsigned bus, unsigned devfn, unsigned reg, unsigned v)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x0C;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.cx = v;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    return 1;
}

int main(int argc, char **argv)
{
    int size = 4;
    int i = 1;
    unsigned bus, dev, fn, reg, devfn;
    unsigned long val;
    unsigned lo, hi;
    unsigned char b;

    if (argc >= 2 && (argv[1][0] == '/' || argv[1][0] == '-')) {
        if (argv[1][1] == 'b' || argv[1][1] == 'B') size = 1;
        else if (argv[1][1] == 'w' || argv[1][1] == 'W') size = 2;
        i = 2;
    }
    if (argc < i + 4) {
        printf("usage: PCIW [/B|/W] bus dev fn reg [val]   (all hex)\n");
        return 1;
    }
    bus = (unsigned)strtoul(argv[i], NULL, 16);
    dev = (unsigned)strtoul(argv[i + 1], NULL, 16);
    fn  = (unsigned)strtoul(argv[i + 2], NULL, 16);
    reg = (unsigned)strtoul(argv[i + 3], NULL, 16);
    devfn = (dev << 3) | fn;

    if (argc > i + 4) {
        val = strtoul(argv[i + 4], NULL, 16);
        if (size == 1) {
            if (!wb(bus, devfn, reg, (unsigned char)val)) goto fail;
        } else if (size == 2) {
            if (!ww(bus, devfn, reg, (unsigned)val)) goto fail;
        } else {
            if (!ww(bus, devfn, reg, (unsigned)(val & 0xFFFFUL))) goto fail;
            if (!ww(bus, devfn, reg + 2, (unsigned)(val >> 16))) goto fail;
        }
    }

    if (size == 1) {
        b = 0xFF;
        if (!rb(bus, devfn, reg, &b)) goto fail;
        printf("%02X:%02X.%X reg %02X = %02X\n", bus, dev, fn, reg, b);
    } else if (size == 2) {
        lo = 0xFFFF;
        if (!rw(bus, devfn, reg, &lo)) goto fail;
        printf("%02X:%02X.%X reg %02X = %04X\n", bus, dev, fn, reg, lo);
    } else {
        lo = hi = 0xFFFF;
        if (!rw(bus, devfn, reg, &lo)) goto fail;
        if (!rw(bus, devfn, reg + 2, &hi)) goto fail;
        val = ((unsigned long)hi << 16) | lo;
        printf("%02X:%02X.%X reg %02X = %08lX\n", bus, dev, fn, reg, val);
    }
    return 0;

fail:
    printf("PCI BIOS call failed\n");
    return 1;
}
