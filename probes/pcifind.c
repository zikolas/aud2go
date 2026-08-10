/* PCIFIND.C -- does the PCI BIOS "find device" call see a hot-enabled card?
 *
 *   PCIFIND vvvv dddd      (hex vendor, device)
 *   PCIFIND                defaults to 1102 0008 (Audigy 2 ZS Notebook)
 *
 * INT 1Ah AH=B1h AL=02h is how MPXPlay/VSBPCM looks for its cards.  Some
 * BIOSes answer it from a table built at POST rather than by scanning live,
 * in which case a CardBus card powered up after boot is invisible to it even
 * though direct config reads (AL=08h) work fine.  This prints both.
 *
 * 16-bit real mode, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

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

int main(int argc, char **argv)
{
    union REGS r;
    unsigned ven, did;

    ven = 0x1102;
    did = 0x0008;
    if (argc >= 3) {
        ven = (unsigned)strtoul(argv[1], NULL, 16);
        did = (unsigned)strtoul(argv[2], NULL, 16);
    }

    printf("looking for %04X:%04X\n", ven, did);

    /* AL=02h  PCI_FIND_DEVICE -- what VSBPCM uses */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x02;
    r.x.cx = did;
    r.x.dx = ven;
    r.x.si = 0;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        printf("FIND_DEVICE (AL=02h): NOT FOUND (ah=%02X cf=%d)\n",
               r.h.ah, (int)(r.x.cflag ? 1 : 0));
    else
        printf("FIND_DEVICE (AL=02h): found at bus %02X devfn %02X\n",
               r.h.bh, r.h.bl);

    /* AL=03h  PCI_FIND_CLASS for multimedia audio (class 0401) */
    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x03;
    r.x.cx = 0x0401;
    r.x.si = 0;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        printf("FIND_CLASS  (AL=03h): NOT FOUND (ah=%02X)\n", r.h.ah);
    else
        printf("FIND_CLASS  (AL=03h): found at bus %02X devfn %02X\n",
               r.h.bh, r.h.bl);

    /* direct config read of 01:00.0 -- what our own tools use */
    printf("direct read 01:00.0 vendor=%04X device=%04X\n",
           cfg_rw(0x01, 0x00, 0x00), cfg_rw(0x01, 0x00, 0x02));
    return 0;
}
