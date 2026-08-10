/* CBIO.C -- isolate where a CardBus I/O cycle dies.
 *
 * Run after CBGO has brought the socket up.  Three staged reads of the same
 * address, each preceded by a breadcrumb flushed to CBIO.LOG:
 *
 *   A  bridge I/O decode OFF, card I/O ON   -> nobody claims it; expect FF
 *   B  bridge I/O decode ON,  card I/O OFF  -> bridge claims, card does not:
 *                                              tests the master-abort path
 *   C  bridge I/O decode ON,  card I/O ON   -> the real access
 *
 * Where it wedges is the answer:
 *   dies at A -> the chipset, nothing to do with CardBus
 *   dies at B -> the bridge cannot complete an unclaimed downstream cycle
 *   dies at C -> the card claims the cycle and never finishes it
 *
 * 16-bit real mode, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

static unsigned g_io   = 0x1400;
static unsigned g_bbus = 0x00, g_bdev = 0x02, g_bfn = 0x00;  /* bridge */
static unsigned g_cbus = 0x01, g_cdev = 0x00;                /* card   */

static char g_msg[160];

static void logstr(char *s)
{
    FILE *f;

    printf("%s\n", s);
    f = fopen("CBIO.LOG", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
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

static unsigned long cfg_rd(unsigned bus, unsigned devfn, unsigned reg)
{
    unsigned long hi;

    hi = cfg_rw(bus, devfn, reg + 2);
    return (hi << 16) | cfg_rw(bus, devfn, reg);
}

static unsigned bdevfn(void) { return (g_bdev << 3) | g_bfn; }
static unsigned cdevfn(void) { return (g_cdev << 3); }

static void show_state(char *when)
{
    sprintf(g_msg, "[%s] card BAR0=%08lX cmd=%04X | bridge cmd=%04X "
                   "io0=%08lX-%08lX bctl=%04X sec-sts=%04X", when,
            cfg_rd(g_cbus, cdevfn(), 0x10),
            cfg_rw(g_cbus, cdevfn(), 0x04),
            cfg_rw(g_bbus, bdevfn(), 0x04),
            cfg_rd(g_bbus, bdevfn(), 0x2C),
            cfg_rd(g_bbus, bdevfn(), 0x30),
            cfg_rw(g_bbus, bdevfn(), 0x3E),
            cfg_rw(g_bbus, bdevfn(), 0x16));
    logstr(g_msg);
}

/* clear the sticky secondary-status error bits by writing them back */
static void clear_secsts(void)
{
    cfg_ww(g_bbus, bdevfn(), 0x16, 0xF900);
}

static void stage(char *name, int bridge_io, int card_io)
{
    unsigned v, bcmd, ccmd;

    bcmd = cfg_rw(g_bbus, bdevfn(), 0x04);
    ccmd = cfg_rw(g_cbus, cdevfn(), 0x04);
    bcmd = bridge_io ? (bcmd | 0x0001) : (bcmd & ~0x0001);
    ccmd = card_io   ? (ccmd | 0x0001) : (ccmd & ~0x0001);
    cfg_ww(g_bbus, bdevfn(), 0x04, bcmd);
    cfg_ww(g_cbus, cdevfn(), 0x04, ccmd);
    clear_secsts();

    sprintf(g_msg, "STAGE %s: bridge-io=%d card-io=%d, ABOUT TO READ %04X",
            name, bridge_io, card_io, g_io);
    logstr(g_msg);

    v = inp(g_io);

    sprintf(g_msg, "STAGE %s: read %04X = %02X, sec-sts=%04X%s",
            name, g_io, v, cfg_rw(g_bbus, bdevfn(), 0x16),
            (cfg_rw(g_bbus, bdevfn(), 0x16) & 0x2000) ? " MASTER-ABORT" : "");
    logstr(g_msg);
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            continue;
        if (!strnicmp(argv[i] + 1, "IO=", 3))
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
    }

    logstr("--- CBIO start ---");
    show_state("initial");

    if (cfg_rw(g_cbus, cdevfn(), 0x00) != 0x1102) {
        logstr("card not present at 01:00.0 -- run CBGO first");
        return 1;
    }

    stage("A", 0, 1);
    stage("B", 1, 0);
    stage("C", 1, 1);

    show_state("final");
    logstr("--- CBIO done ---");
    return 0;
}
