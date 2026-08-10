/* MPUTEST.C -- is there a live MPU-401 UART on the Audigy?
 *
 *   MPUTEST            probe both MPU ports
 *   MPUTEST /N         also play a short note (GM piano, middle C) on ch 1
 *
 * On Audigy the MPU-401 registers are NOT direct I/O -- they live in the PTR
 * indexed register file (emu10k1.h: "For Audigy, MPU port move to 0x70-0x74
 * ptr register"):
 *      A_MUDATA1 0x70 / A_MUCMD1 0x71   MPU on the card (via the game port)
 *      A_MUDATA2 0x72 / A_MUCMD2 0x73   MPU on the Audigy Drive
 * so they are reachable through the ordinary BAR with no extra resources.
 *
 * A real MPU-401 answers RESET (0xFF) and ENTER-UART (0x3F) with ACK 0xFE.
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define EMU_PTR             0x00
#define EMU_DATA            0x04

#define A_MUDATA1           0x70
#define A_MUCMD1            0x71
#define A_MUDATA2           0x72
#define A_MUCMD2            0x73

#define MUCMD_RESET         0xFF
#define MUCMD_ENTERUARTMODE 0x3F
#define MUSTAT_IRDYN        0x80    /* 0 = data / ACK available to read   */
#define MUSTAT_ORDYN        0x40    /* 0 = can accept a command or data   */
#define MPU_ACK             0xFE

unsigned long ind(unsigned port);
#pragma aux ind =           \
    0x66 0xED  0x66 0x8B 0xD0  0x66 0xC1 0xEA 0x10  \
    parm [dx] value [dx ax] modify [dx ax];

void outd(unsigned port, unsigned long val);
#pragma aux outd =          \
    0x66 0x0F 0xB7 0xC0  0x66 0xC1 0xE2 0x10  0x66 0x0B 0xC2  \
    0x8B 0xD1  0x66 0xEF  \
    parm [cx] [dx ax] modify [dx ax];

static unsigned g_io = 0;

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

static unsigned find_card(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x02;
    r.x.cx = 0x0008;
    r.x.dx = 0x1102;
    r.x.si = 0;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    return cfg_rw(r.h.bh, r.h.bl, 0x10) & 0xFFFC;
}

static unsigned char ptr_rb(unsigned reg)
{
    outd(g_io + EMU_PTR, (unsigned long)reg << 16);
    return (unsigned char)(ind(g_io + EMU_DATA) & 0xFF);
}

static void ptr_wb(unsigned reg, unsigned char v)
{
    outd(g_io + EMU_PTR, (unsigned long)reg << 16);
    outd(g_io + EMU_DATA, (unsigned long)v);
}

static void spin(int n)
{
    int i;
    for (i = 0; i < n; i++)
        inp(0x80);
}

/* wait for the UART to be able to accept a byte */
static int wait_ready(unsigned statreg)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (!(ptr_rb(statreg) & MUSTAT_ORDYN))
            return 1;
        spin(40);
    }
    return 0;
}

/* wait for a byte to be readable, return it (-1 on timeout) */
static int read_byte(unsigned statreg, unsigned datareg)
{
    int i;
    for (i = 0; i < 2000; i++) {
        if (!(ptr_rb(statreg) & MUSTAT_IRDYN))
            return ptr_rb(datareg);
        spin(40);
    }
    return -1;
}

static int probe(char *name, unsigned datareg, unsigned cmdreg)
{
    int st, ack;

    st = ptr_rb(cmdreg);
    printf("%s (ptr %02X/%02X): status %02X", name, datareg, cmdreg, st);
    if (st == 0xFF) {
        printf("  -- no UART here\n");
        return 0;
    }
    printf("\n");

    if (!wait_ready(cmdreg)) {
        printf("  UART never accepted a command (ORDYN stuck)\n");
        return 0;
    }
    ptr_wb(cmdreg, MUCMD_RESET);
    ack = read_byte(cmdreg, datareg);
    printf("  RESET      -> %s\n",
           (ack < 0) ? "no reply" : (ack == MPU_ACK) ? "ACK (FE)" : "unexpected");
    if (ack != MPU_ACK)
        return 0;

    if (!wait_ready(cmdreg))
        return 0;
    ptr_wb(cmdreg, MUCMD_ENTERUARTMODE);
    ack = read_byte(cmdreg, datareg);
    printf("  UART mode  -> %s\n",
           (ack < 0) ? "no reply" : (ack == MPU_ACK) ? "ACK (FE)" : "unexpected");
    return (ack == MPU_ACK);
}

static void send(unsigned datareg, unsigned cmdreg, unsigned char b)
{
    wait_ready(cmdreg);
    ptr_wb(datareg, b);
}

int main(int argc, char **argv)
{
    int i, note = 0, ok1, ok2;

    for (i = 1; i < argc; i++)
        if (!stricmp(argv[i], "/N") || !stricmp(argv[i], "-N"))
            note = 1;

    g_io = find_card();
    if (!g_io) {
        printf("Audigy not found - run AUD2GO first.\n");
        return 1;
    }
    printf("Audigy at I/O %04X; MPU-401 lives in PTR space on this chip.\n\n",
           g_io);

    ok1 = probe("MPU1 card/gameport", A_MUDATA1, A_MUCMD1);
    printf("\n");
    ok2 = probe("MPU2 Audigy Drive ", A_MUDATA2, A_MUCMD2);

    if (note && (ok1 || ok2)) {
        unsigned d = ok1 ? A_MUDATA1 : A_MUDATA2;
        unsigned c = ok1 ? A_MUCMD1  : A_MUCMD2;
        printf("\nsending a note on %s...\n", ok1 ? "MPU1" : "MPU2");
        send(d, c, 0xC0); send(d, c, 0x00);        /* program change: piano */
        send(d, c, 0x90); send(d, c, 60); send(d, c, 100);  /* note on  */
        spin(30000); spin(30000); spin(30000);
        send(d, c, 0x80); send(d, c, 60); send(d, c, 0);    /* note off */
        printf("done - anything attached to the MIDI out should have sounded.\n");
    }

    if (!ok1 && !ok2)
        printf("\nNo MPU-401 UART responded.\n");
    return 0;
}
