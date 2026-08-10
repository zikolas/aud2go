/* CBTEST.C -- probe an EMU10K on CardBus with a chosen I/O access width.
 *
 * CBIO proved the bridge and chipset are fine: with the card's I/O decode
 * off, a read of its window master-aborts and returns FF cleanly.  The hang
 * only happens once the card claims the cycle, so the card asserts DEVSEL
 * but never completes.  Every EMU10K driver uses 32-bit dword accesses; a
 * byte read may simply never be answered.
 *
 *   CBTEST /D     32-bit dword read of PTR, INTE, HCFG, WC     (the real test)
 *   CBTEST /W     16-bit word read of PTR
 *   CBTEST /B     8-bit byte read of PTR       (known to hang -- control)
 *   CBTEST /P     posted 32-bit WRITE only, then look at the socket state
 *                 register (which lives in the bridge, not behind it)
 *
 *   /IO=1400 /SB=D000
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms -3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <ctype.h>
#include <dos.h>

#define EMU_PTR         0x00
#define EMU_DATA        0x04
#define EMU_IPR         0x08
#define EMU_INTE        0x0C
#define EMU_WC          0x10
#define EMU_HCFG        0x14

#define CB_SOCKET_STATE 0x08
#define CB_IREQCINT     0x00000040UL

/* 32-bit port I/O built from raw opcodes so this still assembles as
 * ordinary 16-bit real-mode code.
 *   66 ED           in   eax, dx
 *   66 8B D0        mov  edx, eax
 *   66 C1 EA 10     shr  edx, 16      -> long returned in DX:AX
 */
unsigned long ind(unsigned port);
#pragma aux ind =           \
    0x66 0xED               \
    0x66 0x8B 0xD0          \
    0x66 0xC1 0xEA 0x10     \
    parm [dx]               \
    value [dx ax]           \
    modify [dx ax];

/* The value arrives as DX:AX (high:low); only the low halves of EAX/EDX are
 * defined, so zero-extend AX before merging or stray high bits leak in. */
void outd(unsigned port, unsigned long val);
#pragma aux outd =          \
    0x66 0x0F 0xB7 0xC0     /* movzx eax, ax  ; eax = val low     */ \
    0x66 0xC1 0xE2 0x10     /* shl   edx, 16  ; edx = val high<<16*/ \
    0x66 0x0B 0xC2          /* or    eax, edx ; eax = full value  */ \
    0x8B 0xD1               /* mov   dx, cx   ; dx  = port        */ \
    0x66 0xEF               /* out   dx, eax                      */ \
    parm [cx] [dx ax]       \
    modify [dx ax];

static unsigned g_io = 0x1400;
static unsigned g_sockseg = 0xD000;
static char g_msg[160];

static void logstr(char *s)
{
    FILE *f;

    printf("%s\n", s);
    f = fopen("CBTEST.LOG", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}

static unsigned long sock_rd(unsigned reg)
{
    unsigned char __far *p;
    unsigned long v;
    int i;

    p = (unsigned char __far *)MK_FP(g_sockseg, reg);
    v = 0;
    for (i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
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

static void rd32(char *name, unsigned off)
{
    unsigned long v;

    sprintf(g_msg, "ABOUT TO DWORD-READ %s (io+%02X)", name, off);
    logstr(g_msg);
    v = ind(g_io + off);
    sprintf(g_msg, "  %s = %08lX", name, v);
    logstr(g_msg);
}

int main(int argc, char **argv)
{
    int i, mode;
    unsigned long v, st0, st1;
    unsigned w;

    mode = 'D';
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            continue;
        if (!strnicmp(argv[i] + 1, "IO=", 3))
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else if (!strnicmp(argv[i] + 1, "SB=", 3))
            g_sockseg = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else
            mode = toupper(argv[i][1]);
    }

    logstr("--- CBTEST start ---");
    sprintf(g_msg, "mode=%c io=%04X sockseg=%04X  card cmd=%04X status=%04X",
            mode, g_io, g_sockseg,
            cfg_rw(0x01, 0x00, 0x04), cfg_rw(0x01, 0x00, 0x06));
    logstr(g_msg);

    switch (mode) {
    case 'D':
        rd32("PTR ", EMU_PTR);
        rd32("INTE", EMU_INTE);
        rd32("HCFG", EMU_HCFG);
        logstr("ABOUT TO TEST WALL CLOCK (dword)");
        v = ind(g_io + EMU_WC);
        for (i = 0; i < 10000; i++)
            inp(0x80);
        st0 = ind(g_io + EMU_WC);
        sprintf(g_msg, "  WC %08lX -> %08lX  %s", v, st0,
                (v != st0) ? "TICKING - chip is alive" : "STATIC");
        logstr(g_msg);
        break;

    case 'W':
        logstr("ABOUT TO WORD-READ io+00");
        w = inpw(g_io + EMU_PTR);
        sprintf(g_msg, "  word = %04X", w);
        logstr(g_msg);
        break;

    case 'B':
        logstr("ABOUT TO BYTE-READ io+00");
        w = inp(g_io + EMU_PTR);
        sprintf(g_msg, "  byte = %02X", w);
        logstr(g_msg);
        break;

    case 'P':
        st0 = sock_rd(CB_SOCKET_STATE);
        sprintf(g_msg, "socket state before = %08lX (IREQ %s)",
                st0, (st0 & CB_IREQCINT) ? "HIGH" : "low");
        logstr(g_msg);
        logstr("ABOUT TO POSTED-WRITE io+0C (INTE) = 4");
        outd(g_io + EMU_INTE, 4UL);
        logstr("  write returned -- posted writes work");
        st1 = sock_rd(CB_SOCKET_STATE);
        sprintf(g_msg, "socket state after  = %08lX (IREQ %s) %s",
                st1, (st1 & CB_IREQCINT) ? "HIGH" : "low",
                (st0 != st1) ? "CHANGED - card got the write" : "no change");
        logstr(g_msg);
        logstr("ABOUT TO POSTED-WRITE io+0C (INTE) = 0");
        outd(g_io + EMU_INTE, 0UL);
        logstr("  second write returned");
        break;

    default:
        logstr("unknown mode");
        return 1;
    }

    logstr("--- CBTEST done ---");
    return 0;
}
