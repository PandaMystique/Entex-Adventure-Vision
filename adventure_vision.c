/*
 * ============================================================================
 *  ENTEX ADVENTURE VISION EMULATOR v15 — ACCURACY IMPROVEMENTS
 * ============================================================================
 *
 *  Hardware (Dan Boris tech specs / MEGA research):
 *    - Intel 8048 @ 733 KHz (11 MHz / 15), 64B IRAM, 1K BIOS ROM
 *    - XRAM: 4 × 256B, bank select via P1 bits 0-1 (AV custom decode)
 *    - P2.0-P2.3 = cartridge ROM A8-A11, P2.4-P2.7 = sound/LED control
 *    - VRAM: 150 cols × 5 bytes, banks 1-3, offset $06 per bank
 *    - Pixel logic: bit=1 = LED OFF, bit=0 = LED ON (inverted)
 *    - Register 0 = bottom, Register 4 = top (BIOS MOVEY: +A = UP)
 *    - T1 = mirror position sensor, 15 fps
 *    - Buttons via P1.3-P1.7 (active-LOW, matrix encoded)
 *    - Sound: COP411L @ ~54.4 kHz (217.77 kHz RC / 4), commands via P2
 *
 *  v15 improvements (over v14):
 *    - Full COP411L savestate (phase_acc, steps, cur_freq, etc.)
 *    - Mid-frame column scan mode (F3 toggle) for accuracy testing
 *    - Audio profiles: raw/speaker/headphone (F4 cycle)
 *    - Configurable LED gamma, phosphor decay, timing via advision.ini
 *    - Integer scaling mode (F6 toggle)
 *    - Scanline effect overlay (F9 toggle in-game)
 *    - Stats overlay: FPS, cycles, pixels lit (~ key toggle)
 *    - Enhanced debugger: run-to-address (F10 addr), XRAM watchpoints
 *    - Enriched headless: --frames N --input UDLR1234 --dump --test
 *    - Built-in self-test suite (--test)
 *
 *  v14 features:
 *    - Rewind, WAV recording, screenshot BMP, drag & drop ROM loading
 *    - Config file, CLI options, MSVC portability, per-game control hints
 *
 *  v13 features:
 *    - Full COP411L behavioral sound emulation with LFSR noise, pitch
 *      slides, multi-segment volume, and all 13 documented sound effects
 *    - Column-by-column display rendering synchronized with mirror rotation
 *    - Dynamic T1 mirror pulse timing for accurate BIOS sync
 *    - P2-based cartridge ROM addressing
 *
 *  Build:
 *    gcc -O2 -DUSE_SDL -o advision adventure_vision.c -lSDL2 -lm
 *  With embedded ROMs + cover art:
 *    gcc -O2 -DUSE_SDL -DEMBED_ROMS -DEMBED_COVERS -o advision adventure_vision.c -lSDL2 -lm
 *  Usage:
 *    ./advision [bios.rom game.rom]
 *    Without args: scans current dir for ROMs and shows game selector.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>

/* Portable strcasestr for MSVC and other non-GNU systems */
#if defined(_MSC_VER) || !defined(__GLIBC__)
static char *strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}
#endif

#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif

#ifdef USE_SDL
#include <SDL2/SDL.h>
/* Convenience macros for audio thread safety */
#define AUDIO_LOCK(av)   do { if ((av)->adev) SDL_LockAudioDevice((SDL_AudioDeviceID)(av)->adev); } while(0)
#define AUDIO_UNLOCK(av) do { if ((av)->adev) SDL_UnlockAudioDevice((SDL_AudioDeviceID)(av)->adev); } while(0)
/* Ensure key defines exist (some minimal SDL builds miss these) */
#ifndef SDLK_F3
#define SDLK_F3 (SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F3))
#endif
#ifndef SDLK_F4
#define SDLK_F4 (SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F4))
#endif
#ifndef SDLK_F6
#define SDLK_F6 (SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F6))
#endif
#ifndef SDLK_BACKQUOTE
#define SDLK_BACKQUOTE SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_GRAVE)
#endif
#endif

#ifdef EMBED_ROMS
#include "embedded_roms.h"
#endif

#ifdef EMBED_COVERS
#include "cover_art.h"
#endif

/* ---- Configuration ---- */
#define CPU_CLK         733333  /* 11 MHz / 15 — Daniel Boris doc §1.0 */
#define FPS             15
#define CYCLES_PER_FR   48889  /* 733333 / 15, rounded — real AV timing */
#define SW              150
#define SH              40
#define SCALE           5
#define LED_SIZE        4       /* lit portion of each SCALE×SCALE cell */
#define WIN_W           (SW * SCALE)
#define WIN_H           (SH * SCALE)

/* Menu logical dimensions (larger than game for info panel) */
#define MENU_LW         700
#define MENU_LH         460

#define IRAM_SZ         64
#define XRAM_SZ         1024      /* 4 banks × 256 */
#define IROM_SZ         1024
#define EROM_SZ         4096

#define AUDIO_RATE      44100
#define AUDIO_SAMPLES   512
#define MAX_BP          16

/* Rewind buffer: stores snapshots of CPU+RAM state */
#define REWIND_FRAMES   120   /* 8 seconds at 15fps */

/* Audio filter profiles */
#define AUDIO_RAW       0   /* no filter */
#define AUDIO_SPEAKER   1   /* single-pole LP ~4kHz + soft clip */
#define AUDIO_HEADPHONE 2   /* gentler LP ~8kHz, no clip */
#define AUDIO_PROFILES  3
static const char *audio_profile_names[] = {"Raw","Speaker","Headphone"};
static const float audio_lp_alpha[] = {1.0f, 0.45f, 0.7f};

/* Default T1 sensor pulse timing (configurable via ini) */
#define DEF_T1_START    200
#define DEF_T1_END      400
#define DEF_PHOSPHOR    0.45f
#define DEF_LED_GAMMA   1.0f

/* ---- Forward decl ---- */
typedef struct AV AV;
static void av_port_write(AV *av, uint8_t port, uint8_t val);
static uint8_t av_port_read(AV *av, uint8_t port);

/* ============================================================================
 *  INTEL 8048 CPU
 * ========================================================================== */

typedef struct {
    uint8_t  A;
    uint16_t PC;
    uint8_t  PSW, SP;
    bool     MB, C, AC, F0, F1, BS;
    uint8_t  timer;
    bool     timer_en, counter_en, timer_ovf, tcnti_en;
    bool     t0, t1;            /* T0/T1 test pins */
    uint8_t  P1, P2, BUS;
    bool     irq_en, irq_pend, in_irq;
    uint8_t  ei_delay;  /* post-EI delay: >0 = skip IRQ dispatch */
    uint8_t  iram[IRAM_SZ];
    uint8_t  irom[IROM_SZ];
    uint8_t  erom[EROM_SZ];
    uint8_t  xram[XRAM_SZ];
    uint64_t cycles;
    int      tpre;
} I8048;

static inline uint8_t *R(I8048 *c, uint8_t r) {
    return &c->iram[(c->BS ? 24 : 0) + (r & 7)];
}
static inline void bpsw(I8048 *c) {
    c->PSW = (c->C<<7)|(c->AC<<6)|(c->F0<<5)|(c->BS<<4)|(c->SP&7);
}
static inline void push8(I8048 *c) {
    uint8_t a = 8 + c->SP * 2;
    c->iram[a & (IRAM_SZ-1)]     = c->PC & 0xFF;
    c->iram[(a+1) & (IRAM_SZ-1)] = ((c->PC>>8)&0x0F)|(c->PSW&0xF0);
    c->SP = (c->SP+1) & 7;
}
static inline void pop_pc(I8048 *c) {
    c->SP = (c->SP-1) & 7;
    uint8_t a = 8 + c->SP * 2;
    c->PC = c->iram[a & (IRAM_SZ-1)] | ((c->iram[(a+1) & (IRAM_SZ-1)]&0x0F)<<8);
}
static inline void pop_pc_psw(I8048 *c) {
    c->SP = (c->SP-1) & 7;
    uint8_t a = 8 + c->SP * 2;
    c->PC  = c->iram[a & (IRAM_SZ-1)] | ((c->iram[(a+1) & (IRAM_SZ-1)]&0x0F)<<8);
    c->PSW = (c->iram[(a+1) & (IRAM_SZ-1)]&0xF0)|(c->PSW&0x0F);
    c->C=(c->PSW>>7)&1; c->AC=(c->PSW>>6)&1;
    c->F0=(c->PSW>>5)&1; c->BS=(c->PSW>>4)&1;
}

/* ROM addressing:
 * Real hardware: P2 bits 0-3 drive cartridge A8-A11, CPU bus provides A0-A7.
 * But the BIOS always synchronizes P2 with PC before external jumps, so
 * using the full 12-bit address from PC is equivalent and more robust
 * (P2 is temporarily clobbered during sound command transmission). */
static inline uint8_t rom_rd(I8048 *c, uint16_t a) {
    a &= 0xFFF;
    if (a < IROM_SZ && !(c->P1 & 0x04))
        return c->irom[a];
    return c->erom[a & (EROM_SZ-1)];
}

static inline uint8_t ft(I8048 *c) {
    uint8_t v = rom_rd(c, c->PC);
    /* PC is a standard 12-bit counter — all bits increment normally.
     * Note: A11/MB interaction only applies to JMP/CALL destinations,
     * NOT to sequential fetch. MAME confirms full 12-bit increment. */
    c->PC = (c->PC + 1) & 0xFFF;
    return v;
}
static inline uint8_t xram_rd(I8048 *c, uint8_t addr) {
    uint16_t full = ((uint16_t)(c->P1 & 0x03) << 8) | addr;
    return c->xram[full & (XRAM_SZ-1)];
}
static inline void xram_wr(I8048 *c, uint8_t addr, uint8_t val) {
    uint16_t full = ((uint16_t)(c->P1 & 0x03) << 8) | addr;
    c->xram[full & (XRAM_SZ-1)] = val;
}

/* Forward declaration for LED register emulation (defined in display section) */
static int led_reg_decode(uint8_t p2);
/* Forward-declared: latch XRAM read data to LED register (AV not yet defined) */
static void av_led_latch(AV *av, uint8_t p2, uint8_t data);

static int i8048_exec(I8048 *c, AV *sys) {
    uint16_t op_pc = c->PC; /* save for debug before auto-increment */
    uint8_t op = ft(c), t, *r;
    uint16_t t16;
    int cy = 1;

    switch (op) {
    case 0x00: break; /* NOP */

    /* MOV */
    case 0xF8:case 0xF9:case 0xFA:case 0xFB:case 0xFC:case 0xFD:case 0xFE:case 0xFF:
        c->A = *R(c, op&7); break;
    case 0xA8:case 0xA9:case 0xAA:case 0xAB:case 0xAC:case 0xAD:case 0xAE:case 0xAF:
        *R(c, op&7) = c->A; break;
    case 0x23: c->A = ft(c); cy=2; break;
    case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
        *R(c, op&7) = ft(c); cy=2; break;
    case 0xF0:case 0xF1: c->A = c->iram[*R(c,op&1)&(IRAM_SZ-1)]; break;
    case 0xA0:case 0xA1: c->iram[*R(c,op&1)&(IRAM_SZ-1)] = c->A; break;
    case 0xB0:case 0xB1: c->iram[*R(c,op&1)&(IRAM_SZ-1)] = ft(c); cy=2; break;

    /* XCH / XCHD */
    case 0x28:case 0x29:case 0x2A:case 0x2B:case 0x2C:case 0x2D:case 0x2E:case 0x2F:
        r=R(c,op&7); t=c->A; c->A=*r; *r=t; break;
    case 0x20:case 0x21:{uint8_t a=*R(c,op&1)&(IRAM_SZ-1);t=c->iram[a];c->iram[a]=c->A;c->A=t;break;}
    case 0x30:case 0x31:{uint8_t a=*R(c,op&1)&(IRAM_SZ-1);t=c->A&0xF;c->A=(c->A&0xF0)|(c->iram[a]&0xF);c->iram[a]=(c->iram[a]&0xF0)|t;break;}

    /* ADD */
    case 0x68:case 0x69:case 0x6A:case 0x6B:case 0x6C:case 0x6D:case 0x6E:case 0x6F:
        t=*R(c,op&7);t16=c->A+t;c->AC=((c->A&0xF)+(t&0xF))>0xF;c->C=t16>0xFF;c->A=t16;break;
    case 0x03:t=ft(c);t16=c->A+t;c->AC=((c->A&0xF)+(t&0xF))>0xF;c->C=t16>0xFF;c->A=t16;cy=2;break;
    case 0x60:case 0x61:t=c->iram[*R(c,op&1)&(IRAM_SZ-1)];t16=c->A+t;c->AC=((c->A&0xF)+(t&0xF))>0xF;c->C=t16>0xFF;c->A=t16;break;

    /* ADDC */
    case 0x78:case 0x79:case 0x7A:case 0x7B:case 0x7C:case 0x7D:case 0x7E:case 0x7F:
        t=*R(c,op&7);t16=c->A+t+c->C;c->AC=((c->A&0xF)+(t&0xF)+c->C)>0xF;c->C=t16>0xFF;c->A=t16;break;
    case 0x13:t=ft(c);t16=c->A+t+c->C;c->AC=((c->A&0xF)+(t&0xF)+c->C)>0xF;c->C=t16>0xFF;c->A=t16;cy=2;break;
    case 0x70:case 0x71:t=c->iram[*R(c,op&1)&(IRAM_SZ-1)];t16=c->A+t+c->C;c->AC=((c->A&0xF)+(t&0xF)+c->C)>0xF;c->C=t16>0xFF;c->A=t16;break;

    /* Logic: ANL, ORL, XRL */
    case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:c->A&=*R(c,op&7);break;
    case 0x53:c->A&=ft(c);cy=2;break;
    case 0x50:case 0x51:c->A&=c->iram[*R(c,op&1)&(IRAM_SZ-1)];break;
    case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F:c->A|=*R(c,op&7);break;
    case 0x43:c->A|=ft(c);cy=2;break;
    case 0x40:case 0x41:c->A|=c->iram[*R(c,op&1)&(IRAM_SZ-1)];break;
    case 0xD8:case 0xD9:case 0xDA:case 0xDB:case 0xDC:case 0xDD:case 0xDE:case 0xDF:c->A^=*R(c,op&7);break;
    case 0xD3:c->A^=ft(c);cy=2;break;
    case 0xD0:case 0xD1:c->A^=c->iram[*R(c,op&1)&(IRAM_SZ-1)];break;

    /* INC / DEC / CLR / CPL */
    case 0x17:c->A++;break;
    case 0x18:case 0x19:case 0x1A:case 0x1B:case 0x1C:case 0x1D:case 0x1E:case 0x1F:(*R(c,op&7))++;break;
    case 0x10:case 0x11:c->iram[*R(c,op&1)&(IRAM_SZ-1)]++;break;
    case 0x07:c->A--;break;
    case 0xC8:case 0xC9:case 0xCA:case 0xCB:case 0xCC:case 0xCD:case 0xCE:case 0xCF:
        (*R(c,op&7))--;break; /* DEC Rr — valid 8048 opcode (MAME confirmed) */
    case 0x27:c->A=0;break;
    case 0x37:c->A=~c->A;break;

    /* DA / SWAP / Rotate */
    case 0x57:if((c->A&0xF)>9||c->AC){t=c->A;c->A+=6;if(c->A<t)c->C=1;}if((c->A>>4)>9||c->C){c->A+=0x60;c->C=1;}break;
    case 0x47:c->A=((c->A&0xF)<<4)|((c->A>>4)&0xF);break;
    case 0xE7:c->A=(c->A<<1)|(c->A>>7);break;         /* RL A */
    case 0xF7:t=c->C;c->C=(c->A>>7)&1;c->A=(c->A<<1)|t;break; /* RLC A */
    case 0x77:c->A=(c->A>>1)|(c->A<<7);break;         /* RR A */
    case 0x67:t=c->C;c->C=c->A&1;c->A=(c->A>>1)|(t<<7);break; /* RRC A */

    /* Flags */
    case 0x97:c->C=0;break;  case 0xA7:c->C=!c->C;break;
    case 0x85:c->F0=0;break; case 0x95:c->F0=!c->F0;break;
    case 0xA5:c->F1=0;break; case 0xB5:c->F1=!c->F1;break;
    case 0xC5:c->BS=0;break; case 0xD5:c->BS=1;break;
    case 0xE5:c->MB=0;break; case 0xF5:c->MB=1;break;

    /* JMP */
    case 0x04:case 0x24:case 0x44:case 0x64:case 0x84:case 0xA4:case 0xC4:case 0xE4:
        t=ft(c);c->PC=((uint16_t)(op&0xE0)<<3)|t;if(c->MB)c->PC|=0x800;cy=2;break;
    case 0xB3:c->PC=(c->PC&0xF00)|rom_rd(c,(c->PC&0xF00)|c->A);cy=2;break; /* JMPP @A */

    /* DJNZ */
    case 0xE8:case 0xE9:case 0xEA:case 0xEB:case 0xEC:case 0xED:case 0xEE:case 0xEF:
        t=ft(c);r=R(c,op&7);(*r)--;if(*r)c->PC=(c->PC&0xF00)|t;cy=2;break;

    /* Conditional jumps */
    case 0xF6:t=ft(c);if(c->C)c->PC=(c->PC&0xF00)|t;cy=2;break;   /* JC */
    case 0xE6:t=ft(c);if(!c->C)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JNC */
    case 0xC6:t=ft(c);if(!c->A)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JZ */
    case 0x96:t=ft(c);if(c->A)c->PC=(c->PC&0xF00)|t;cy=2;break;   /* JNZ */
    case 0x26:t=ft(c);if(!c->t0)c->PC=(c->PC&0xF00)|t;cy=2;break; /* JNT0: T0=0 */
    case 0x36:t=ft(c);if(c->t0)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JT0: T0=1 */
    case 0x46:t=ft(c);if(!c->t1)c->PC=(c->PC&0xF00)|t;cy=2;break; /* JNT1: T1=0 */
    case 0x56:t=ft(c);if(c->t1)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JT1: T1=1 */
    case 0xB6:t=ft(c);if(c->F0)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JF0 */
    case 0x76:t=ft(c);if(c->F1)c->PC=(c->PC&0xF00)|t;cy=2;break;  /* JF1 */
    case 0x16:t=ft(c);if(c->timer_ovf){c->PC=(c->PC&0xF00)|t;c->timer_ovf=0;}cy=2;break; /* JTF */
    case 0x86:t=ft(c);cy=2;break; /* JNI — INT not connected in AV */
    case 0x12:case 0x32:case 0x52:case 0x72:case 0x92:case 0xB2:case 0xD2:case 0xF2:
        t=ft(c);if(c->A&(1<<((op>>5)&7)))c->PC=(c->PC&0xF00)|t;cy=2;break; /* JBb */

    /* CALL / RET */
    case 0x14:case 0x34:case 0x54:case 0x74:case 0x94:case 0xB4:case 0xD4:case 0xF4:
        t=ft(c);bpsw(c);push8(c);c->PC=((uint16_t)(op&0xE0)<<3)|t;if(c->MB)c->PC|=0x800;cy=2;break;
    case 0x83:pop_pc(c);cy=2;break;           /* RET */
    case 0x93:pop_pc_psw(c);c->irq_en=1;c->in_irq=0;cy=2;break; /* RETR */

    /* Interrupts & Timer */
    case 0x05:c->irq_en=1;c->ei_delay=1;break; /* EI + 1-instr delay */
    case 0x15:c->irq_en=0;break;
    case 0x25:c->tcnti_en=1;break;
    case 0x35:c->tcnti_en=0;break;
    case 0x55:c->timer_en=1;c->counter_en=0;c->tpre=0;break;  /* STRT T: start timer, clear prescaler */
    case 0x45:c->counter_en=1;c->timer_en=0;c->tpre=0;break;  /* STRT CNT: start counter, clear prescaler */
    case 0x65:c->timer_en=0;c->counter_en=0;c->tpre=0;break;  /* STOP TCNT: stop and clear prescaler */
    case 0x42:c->A=c->timer;break;
    case 0x62:c->timer=c->A;c->tpre=0;break;  /* MOV T,A: load timer + clear prescaler */

    /* PSW */
    case 0xC7:bpsw(c);c->A=c->PSW;break;
    case 0xD7:c->PSW=c->A;c->C=(c->PSW>>7)&1;c->AC=(c->PSW>>6)&1;c->F0=(c->PSW>>5)&1;c->BS=(c->PSW>>4)&1;c->SP=c->PSW&7;break;

    /* I/O ports */
    case 0x08:c->A=av_port_read(sys,0);cy=2;break;       /* INS A,BUS */
    case 0x02:c->BUS=c->A;av_port_write(sys,0,c->A);cy=2;break;  /* OUTL BUS,A */
    case 0x88:c->BUS|=ft(c);av_port_write(sys,0,c->BUS);cy=2;break; /* ORL BUS,#data */
    case 0x98:c->BUS&=ft(c);av_port_write(sys,0,c->BUS);cy=2;break; /* ANL BUS,#data */
    case 0x09:c->A=av_port_read(sys,1);cy=2;break;       /* IN A,P1 */
    case 0x0A:c->A=av_port_read(sys,2);cy=2;break;       /* IN A,P2 */
    case 0x39:c->P1=c->A;av_port_write(sys,1,c->A);cy=2;break; /* OUTL P1,A */
    case 0x3A:c->P2=c->A;av_port_write(sys,2,c->A);cy=2;break; /* OUTL P2,A */
    case 0x99:c->P1&=ft(c);av_port_write(sys,1,c->P1);cy=2;break; /* ANL P1,#data */
    case 0x9A:c->P2&=ft(c);av_port_write(sys,2,c->P2);cy=2;break; /* ANL P2,#data */
    case 0x89:c->P1|=ft(c);av_port_write(sys,1,c->P1);cy=2;break; /* ORL P1,#data */
    case 0x8A:c->P2|=ft(c);av_port_write(sys,2,c->P2);cy=2;break; /* ORL P2,#data */

    /* MOVX A,@Rr — External RAM read (banked via P1 bits 0-1).
     * Hardware side-effect (§4.3): "The actual write to the LED registers
     * occurs when there is a read from external memory." The data bus value
     * is simultaneously latched into the LED register selected by P2.5-P2.7.
     * This trick allows the BIOS to fill LED regs at max speed. */
    case 0x80:case 0x81:{
        uint8_t xval = xram_rd(c, *R(c, op&1));
        c->A = xval;
        av_led_latch(sys, c->P2, xval);
        cy = 2;
        break;
    }
    case 0x90:case 0x91:xram_wr(c,*R(c,op&1),c->A);cy=2;break;

    /* MOVP */
    case 0xA3:c->A=rom_rd(c,(c->PC&0xF00)|c->A);cy=2;break;
    case 0xE3:c->A=rom_rd(c,0x300|c->A);cy=2;break;

    /* MOVD (8243 port expander — not connected in AV) */
    case 0x0C:case 0x0D:case 0x0E:case 0x0F:c->A=0x0F;cy=2;break;
    case 0x3C:case 0x3D:case 0x3E:case 0x3F:cy=2;break;
    case 0x8C:case 0x8D:case 0x8E:case 0x8F:cy=2;break;
    case 0x9C:case 0x9D:case 0x9E:case 0x9F:cy=2;break;

    case 0x75: break; /* ENT0 CLK */

    default:
        fprintf(stderr,"[8048] Unknown opcode $%02X @ PC=$%03X\n",op,op_pc);
        break;
    }

    c->cycles += cy;

    /* Timer prescaler: increments every 32 cycles */
    if (c->timer_en) {
        c->tpre += cy;
        while (c->tpre >= 32) {
            c->tpre -= 32;
            if (++c->timer == 0) {
                c->timer_ovf = true;
                if (c->tcnti_en && c->irq_en && !c->in_irq)
                    c->irq_pend = true;
            }
        }
    }

    /* IRQ dispatch — 8048 requires 1 instruction after EI before accepting */
    if (c->ei_delay > 0) c->ei_delay--;
    if (c->irq_pend && c->irq_en && !c->in_irq && c->ei_delay == 0) {
        c->irq_pend = false;
        c->in_irq = true;
        c->irq_en = false;
        bpsw(c); push8(c);
        c->PC = 0x007;
    }

    return cy;
}

/* ============================================================================
 *  COP411L SOUND PROCESSOR — BEHAVIORAL EMULATION
 * ============================================================================
 *
 *  The COP411L is a 4-bit microcontroller with 512×8 ROM (mask-programmed
 *  with Entex's sound firmware) and 32×4 RAM. Since the ROM contents are
 *  not publicly dumped, we emulate the documented sound behaviors faithfully.
 *
 *  Audio output: Port G bit 0 (high weight) + Port D bit 0 (low weight)
 *  creates a 2-bit DAC with 3 effective volume levels.
 *
 *  Sound commands (MEGA documentation):
 *    $00 = Control register (loop, volume, delay)
 *    $1x = Continuous noise
 *    $2x = High→low square slide (shooting)
 *    $3x = 5-pitch noise explosion, loops to cmd 2
 *    $4x = Low→high square slide (reward)
 *    $5x = Low→high noise slide (thrusters), loops last pitch
 *    $6x = High→low noise slide (explosion/landing), never loops
 *    $7x = Medium→low square slide (enemy shooting)
 *    $8x = Very fast low→high square (multi-purpose / phone ring)
 *    $9x = Quick low→high square (jump / alarm)
 *    $Ax-$Dx = Unknown, minimal behavior
 *    $Ex,$Fx = Pure tones (16 notes, A#3 to C#5)
 */

/* Musical note frequencies for pure tones (equal temperament, A4=440Hz) */
/* Hardware-measured nominal frequencies from Daniel Boris doc §6.2.
 * These are the actual COP411L output frequencies at 52.6 kHz RC clock,
 * NOT equal-temperament approximations. */
static const float cop411_note_freq[16] = {
    239.23f,  /* 0: ~A#3 */
    253.03f,  /* 1: ~B3 */
    268.53f,  /* 2: ~C4 */
    286.04f,  /* 3: ~C#4 */
    302.48f,  /* 4: ~D4 */
    320.92f,  /* 5: ~D#4 */
    337.38f,  /* 6: ~E4 */
    360.49f,  /* 7: ~F4 */
    381.38f,  /* 8: ~F#4 */
    404.85f,  /* 9: ~G4 */
    424.44f,  /* 10: ~G#4 */
    453.72f,  /* 11: ~A4 */
    478.46f,  /* 12: ~A#4 */
    506.07f,  /* 13: ~B4 */
    537.05f,  /* 14: ~C5 */
    572.08f,  /* 15: ~C#5 */
};

/* Effect step descriptor: frequency + noise flag + duration (in ms) */
typedef struct {
    float freq;       /* Hz (0 = silence) */
    bool  noise;      /* true = LFSR noise, false = square wave */
    int   dur_ms;     /* duration of this step in milliseconds */
    float volume;     /* relative volume 0.0-1.0 */
} SndStep;

#define MAX_SND_STEPS 16

typedef struct {
    /* Control register (persistent across resets, as per COP411L RAM behavior) */
    uint8_t  ctrl_loop;   /* bit 0: 0=play once, 1=loop */
    uint8_t  ctrl_vol;    /* bits 1-2: segment volume control */
    uint8_t  ctrl_fast;   /* bit 3: 0=slow, 1=fast */

    /* Sound command protocol state */
    uint8_t  proto_state; /* 0=idle, 1=got $C0, 2=got hi nib, 3=dispatched */
    uint8_t  proto_hi;    /* high nibble captured from first P2 data write */

    /* Current effect playback */
    bool     active;
    bool     is_noise;
    bool     force_loop;  /* cmd 1: always loops; cmd 6: never loops */
    bool     force_no_loop;
    uint8_t  command;     /* last command (1-F) */

    /* Step sequencer for multi-step effects */
    SndStep  steps[MAX_SND_STEPS];
    int      step_count;
    int      cur_step;
    int      step_samples_left;

    /* Waveform state */
    float    cur_freq;
    uint32_t phase_acc;    /* 32-bit phase accumulator */
    uint32_t phase_inc;    /* phase increment for current frequency */

    /* Pitch slide */
    float    slide_freq_start;
    float    slide_freq_end;
    float    slide_progress; /* 0.0 to 1.0 within current step */

    /* Noise LFSR (15-bit) */
    uint16_t lfsr;

    /* Volume */
    float    seg1_vol;    /* 1st segment volume (from ctrl) */
    float    seg2_vol;    /* 2nd segment volume (from ctrl) */
    float    cur_vol;     /* current output volume */
    int      segment;     /* 0=seg1, 1=seg2 for tones */

    /* Tone segment timing */
    int      seg_samples_total;
    int      seg_samples_left;

    /* For looping effects that transition (cmd 3 → cmd 2) */
    uint8_t  chain_cmd;   /* next command after current finishes */
} COP411L;

/* Calculate phase increment for a given frequency */
static inline uint32_t freq_to_phase_inc(float freq) {
    if (freq <= 0.0f) return 0;
    return (uint32_t)((freq / (float)AUDIO_RATE) * 4294967296.0);
}

/* Clock the 15-bit LFSR noise generator (taps at bits 0 and 1) */
static inline int lfsr_clock(COP411L *snd) {
    uint16_t bit = ((snd->lfsr >> 0) ^ (snd->lfsr >> 1)) & 1;
    snd->lfsr = (snd->lfsr >> 1) | (bit << 14);
    return snd->lfsr & 1;
}

static void cop411_init(COP411L *snd) {
    memset(snd, 0, sizeof(COP411L));
    snd->lfsr = 0x7FFF;  /* Initialize LFSR to non-zero */
    snd->seg1_vol = 1.0f;
    snd->seg2_vol = 0.5f;
}

/* Update volume settings from control register */
static void cop411_update_ctrl_vol(COP411L *snd) {
    /* Doc §6.2: bits 1-2 of control register:
     *   0,0 = low/low  1,0 = high/low  0,1 = high/high  1,1 = high/high
     *   ctrl_vol = (bit2<<1)|bit1: 0=low/low 1=high/low 2,3=high/high */
    switch (snd->ctrl_vol) {
    case 0: snd->seg1_vol = 0.4f; snd->seg2_vol = 0.4f; break;
    case 1: snd->seg1_vol = 1.0f; snd->seg2_vol = 0.4f; break;
    default:snd->seg1_vol = 1.0f; snd->seg2_vol = 1.0f; break;
    }
}

/* Get the speed multiplier from control register */
static float cop411_speed(const COP411L *snd) {
    return snd->ctrl_fast ? 0.5f : 1.0f;  /* Doc §6.2 bit 0: 1=fast (shorter duration) */
}

/* Build effect steps for a given command */
static void cop411_build_effect(COP411L *snd, uint8_t cmd, uint8_t data) {
    (void)data;
    snd->command = cmd;
    snd->active = true;
    snd->cur_step = 0;
    snd->step_count = 0;
    snd->chain_cmd = 0;
    snd->force_loop = false;
    snd->force_no_loop = false;
    snd->segment = 0;
    snd->phase_acc = 0;

    float spd = cop411_speed(snd);

    switch (cmd) {
    case 0x01: {
        /* Continuous noise — always loops regardless of control */
        snd->force_loop = true;
        snd->step_count = 1;
        snd->steps[0] = (SndStep){800.0f, true, (int)(200*spd), 0.8f};
        break;
    }
    case 0x02: {
        /* High→low square slide (shooting: "pew pew") */
        int n = 8;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 1200.0f - (float)i * (900.0f / (float)n);
            snd->steps[i] = (SndStep){f, false, (int)(25*spd), 1.0f - (float)i*0.08f};
        }
        break;
    }
    case 0x03: {
        /* 5-pitch noise explosion, loops → command 2 */
        snd->step_count = 5;
        float pitches[] = {1000.0f, 800.0f, 600.0f, 400.0f, 250.0f};
        for (int i = 0; i < 5; i++)
            snd->steps[i] = (SndStep){pitches[i], true, (int)(60*spd), 1.0f - (float)i*0.12f};
        if (snd->ctrl_loop)
            snd->chain_cmd = 0x02;
        break;
    }
    case 0x04: {
        /* Low→high square slide (reward) */
        int n = 8;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 300.0f + (float)i * (900.0f / (float)n);
            snd->steps[i] = (SndStep){f, false, (int)(30*spd), 0.7f + (float)i*0.04f};
        }
        break;
    }
    case 0x05: {
        /* Low→high noise slide (thrusters) — loops from last pitch */
        int n = 10;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 200.0f + (float)i * (600.0f / (float)n);
            int dur = (int)((40 + i * 8) * spd);
            snd->steps[i] = (SndStep){f, true, dur, 0.6f + (float)i*0.04f};
        }
        /* Loop: continue playing from last pitch */
        snd->force_loop = snd->ctrl_loop;
        break;
    }
    case 0x06: {
        /* High→low noise slide (explosion/landing) — NEVER loops */
        snd->force_no_loop = true;
        int n = 12;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 1200.0f - (float)i * (900.0f / (float)n);
            int dur = (int)((30 + i * 10) * spd);
            snd->steps[i] = (SndStep){f, true, dur, 1.0f - (float)i*0.06f};
        }
        break;
    }
    case 0x07: {
        /* Medium→low square slide (enemy shooting) */
        int n = 6;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 800.0f - (float)i * (500.0f / (float)n);
            snd->steps[i] = (SndStep){f, false, (int)(30*spd), 0.9f - (float)i*0.1f};
        }
        break;
    }
    case 0x08: {
        /* Very fast low→high square (multi-purpose / phone ring when looped) */
        int n = 6;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 400.0f + (float)i * (800.0f / (float)n);
            snd->steps[i] = (SndStep){f, false, (int)(12*spd), 0.8f};
        }
        break;
    }
    case 0x09: {
        /* Quick low→high square (jump / alarm when looped) */
        int n = 8;
        snd->step_count = n;
        for (int i = 0; i < n; i++) {
            float f = 300.0f + (float)i * (600.0f / (float)n);
            snd->steps[i] = (SndStep){f, false, (int)(18*spd), 0.85f};
        }
        break;
    }
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
        /* Unknown commands — approximate as brief pitch change */
        snd->step_count = 1;
        float f = 300.0f + (float)(cmd - 0x0A) * 100.0f;
        snd->steps[0] = (SndStep){f, false, (int)(50*spd), 0.5f};
        break;
    }
    default:
        snd->active = false;
        return;
    }

    /* Start first step */
    if (snd->step_count > 0) {
        SndStep *s = &snd->steps[0];
        snd->cur_freq = s->freq;
        snd->is_noise = s->noise;
        snd->cur_vol = s->volume;
        snd->phase_inc = freq_to_phase_inc(s->freq);
        snd->step_samples_left = (s->dur_ms * AUDIO_RATE) / 1000;
        if (snd->step_samples_left < 1) snd->step_samples_left = 1;
    }
}

/* Start a pure tone (commands $E and $F) */
static void cop411_start_tone(COP411L *snd, uint8_t note) {
    snd->active = true;
    snd->is_noise = false;
    snd->command = 0x0E;
    snd->cur_step = 0;
    snd->step_count = 0;
    snd->chain_cmd = 0;
    snd->force_loop = false;
    snd->force_no_loop = false;

    float freq = cop411_note_freq[note & 0x0F];
    snd->cur_freq = freq;
    snd->phase_inc = freq_to_phase_inc(freq);

    /* Two-segment playback — Doc §6.2: segments have different durations.
     * Fast=0 (slow): seg1=117ms, seg2=240ms
     * Fast=1 (fast): seg1=46ms,  seg2=104ms */
    snd->segment = 0;
    cop411_update_ctrl_vol(snd);
    snd->cur_vol = snd->seg1_vol;

    int seg1_ms = snd->ctrl_fast ? 46 : 117;
    snd->seg_samples_total = (seg1_ms * AUDIO_RATE) / 1000;
    snd->seg_samples_left = snd->seg_samples_total;
}

/* Process a received sound command byte */
static void cop411_command(COP411L *snd, uint8_t cmd_byte) {
    uint8_t cmd = (cmd_byte >> 4) & 0x0F;
    uint8_t data = cmd_byte & 0x0F;

    if (cmd == 0x00) {
        /* Control register — persists across resets (in COP411L RAM) */
        /* Doc §6.1: bit 0 = fast/slow, bits 1-2 = volume, bit 3 = loop */
        snd->ctrl_fast = data & 0x01;
        snd->ctrl_vol  = (data >> 1) & 0x03;
        snd->ctrl_loop = (data >> 3) & 0x01;
        cop411_update_ctrl_vol(snd);
        /* Silence current sound */
        snd->active = false;
        return;
    }

    if (cmd == 0x0E || cmd == 0x0F) {
        cop411_start_tone(snd, data);
        return;
    }

    if (cmd >= 0x01 && cmd <= 0x0D) {
        cop411_build_effect(snd, cmd, data);
        return;
    }
}

/* Generate one audio sample from the COP411L */
static inline float cop411_sample(COP411L *snd) {
    if (!snd->active) return 0.0f;

    float out;
    if (snd->is_noise) {
        /* LFSR noise: clock at the current frequency rate */
        snd->phase_acc += snd->phase_inc;
        /* Clock LFSR on phase overflow (roughly at freq rate) */
        if (snd->phase_acc < snd->phase_inc) {
            lfsr_clock(snd);
        }
        out = (snd->lfsr & 1) ? 1.0f : -1.0f;
    } else {
        /* Square wave */
        snd->phase_acc += snd->phase_inc;
        out = (snd->phase_acc & 0x80000000) ? 1.0f : -1.0f;
    }

    out *= snd->cur_vol;

    /* Advance step sequencer (for SFX commands 1-D) */
    if (snd->step_count > 0) {
        snd->step_samples_left--;
        if (snd->step_samples_left <= 0) {
            snd->cur_step++;
            if (snd->cur_step >= snd->step_count) {
                /* Effect finished */
                if (snd->chain_cmd) {
                    /* Chain to next command (e.g., cmd 3 → cmd 2) */
                    cop411_build_effect(snd, snd->chain_cmd, 0);
                    return out;
                }
                bool should_loop = false;
                if (snd->force_no_loop) should_loop = false;
                else if (snd->force_loop) should_loop = true;
                else should_loop = snd->ctrl_loop;

                if (should_loop) {
                    /* For cmd 5: loop from last step only */
                    if (snd->command == 0x05) {
                        snd->cur_step = snd->step_count - 1;
                    } else {
                        snd->cur_step = 0;
                    }
                } else {
                    snd->active = false;
                    return out;
                }
            }
            /* Load new step (bounds check: defensive against corruption) */
            if (snd->cur_step < 0 || snd->cur_step >= MAX_SND_STEPS) {
                snd->active = false; return out;
            }
            SndStep *s = &snd->steps[snd->cur_step];
            snd->cur_freq = s->freq;
            snd->is_noise = s->noise;
            snd->cur_vol = s->volume;
            snd->phase_inc = freq_to_phase_inc(s->freq);
            snd->step_samples_left = (s->dur_ms * AUDIO_RATE) / 1000;
            if (snd->step_samples_left < 1) snd->step_samples_left = 1;
        }
    } else {
        /* Pure tone: two-segment playback */
        snd->seg_samples_left--;
        if (snd->seg_samples_left <= 0) {
            if (snd->segment == 0) {
                /* Transition to segment 2 */
                snd->segment = 1;
                snd->cur_vol = snd->seg2_vol;
                /* Seg2 has its own duration (Doc §6.2) */
                    { int seg2_ms = snd->ctrl_fast ? 104 : 240;
                      snd->seg_samples_left = (seg2_ms * AUDIO_RATE) / 1000; }
            } else {
                /* Tone finished */
                if (snd->ctrl_loop) {
                    snd->segment = 0;
                    snd->cur_vol = snd->seg1_vol;
                    snd->seg_samples_left = snd->seg_samples_total;
                } else {
                    snd->active = false;
                }
            }
        }
    }

    return out;
}

/* ============================================================================
 *  DISPLAY (Column-by-column rendering)
 * ========================================================================== */

/* POV persistence decay: the spinning mirror sweeps each column once
 * per revolution (~66ms at 15fps). Human eye persistence is ~100ms,
 * so pixels fade over 2-3 frames. Decay 0.45 per frame gives:
 * frame 0: 1.0, frame 1: 0.45, frame 2: 0.20, frame 3: 0.09 */
/* PHOSPHOR_DECAY is now configurable via av->cfg_phosphor */
#define PHOSPHOR_DECAY_DEFAULT  0.45f

typedef struct {
    float   phosphor[SW * SH]; /* 0.0-1.0, POV persistence per LED */
    /* Column snapshot buffer: holds VRAM data at time each column is scanned */
    uint8_t col_data[SW][5];   /* [column][byte 0-4] captured during frame */
    int     cols_captured;     /* how many columns captured this frame */

    /* Hardware LED registers (Daniel Boris doc §4.3):
     * 5 registers x 8 bits = 40 LEDs. Written as a side-effect of MOVX
     * reads: when MOVX A,@Rr executes, the data read from XRAM is
     * simultaneously latched into the LED register selected by P2.5-P2.7.
     * P2.4 rising edge then strobes the LED data to the display. */
    uint8_t led_reg[5];     /* LED registers 0-4 (LEDs 1-8 through 33-40) */
    int     led_col;         /* current column counter (reset on T1 sync) */
    bool    led_active;      /* true if any P2.4 strobes seen this frame */
} AVDisp;

/* Capture a column from current VRAM state (called during frame execution) */
static void disp_capture_column(AVDisp *d, const uint8_t *xram, int col) {
    if ((unsigned)col >= (unsigned)SW) return;
    int bank = 1 + col / 50;
    int offset = 6 + (col % 50) * 5;
    int base = bank * 256 + offset;
    if (base + 4 < XRAM_SZ) {
        for (int i = 0; i < 5; i++)
            d->col_data[col][i] = xram[base + i];
    }
    if (col >= d->cols_captured)
        d->cols_captured = col + 1;
}

/* Decode P2 bits 5-7 to LED register index per hardware spec (§4.3):
 * P2.5 P2.6 P2.7 -> Register (LED numbers from top)
 *   1    0    0  -> 0 (LEDs 1-8)
 *   0    1    0  -> 1 (LEDs 9-16)
 *   1    1    0  -> 2 (LEDs 17-24)
 *   0    0    1  -> 3 (LEDs 25-32)
 *   1    0    1  -> 4 (LEDs 33-40)
 *   other        -> -1 (invalid/unused) */
static int led_reg_decode(uint8_t p2) {
    uint8_t sel = (p2 >> 5) & 7;
    switch (sel) {
    case 4: return 0;  /* 100 */
    case 2: return 1;  /* 010 */
    case 6: return 2;  /* 110 */
    case 1: return 3;  /* 001 */
    case 5: return 4;  /* 101 */
    default: return -1; /* 011, 111, 000 = unused */
    }
}

/* Latch LED registers to display column (called on P2.4 rising edge).
 * This is the hardware-accurate display path: the BIOS fills LED registers
 * via MOVX reads, then strobes P2.4 to advance to the next column. */
static void disp_latch_led_column(AVDisp *d) {
    int col = d->led_col;
    if (col >= 0 && col < SW) {
        for (int i = 0; i < 5; i++)
            d->col_data[col][i] = d->led_reg[i];
        if (col >= d->cols_captured)
            d->cols_captured = col + 1;
    }
    d->led_col++;
    d->led_active = true;
}

/* Update display from captured column data (called at end of frame)
 * Simulates POV persistence: existing pixel brightness decays each frame,
 * then newly lit pixels are set to full brightness. */
static void disp_update(AVDisp *d, float decay) {
    /* Decay existing phosphor (POV persistence fading) */
    for (int i = 0; i < SW * SH; i++) {
        d->phosphor[i] *= decay;
        if (d->phosphor[i] < 0.01f) d->phosphor[i] = 0.0f;
    }

    /* Light up pixels from captured column data */
    int cols = d->cols_captured;
    if (cols > SW) cols = SW;
    for (int col = 0; col < cols; col++) {
        for (int bi = 0; bi < 5; bi++) {
            uint8_t val = d->col_data[col][bi];
            for (int bit = 0; bit < 8; bit++) {
                int y = (4 - bi) * 8 + (7 - bit);
                if ((unsigned)y >= (unsigned)SH) continue;
                if (!(val & (1 << bit)))
                    d->phosphor[col + y * SW] = 1.0f;
            }
        }
    }
    d->cols_captured = 0;
}

/* Get pixel intensity */
static float disp_px(const AVDisp *d, int x, int y) {
    if ((unsigned)x >= (unsigned)SW || (unsigned)y >= (unsigned)SH) return 0.0f;
    return d->phosphor[x + y * SW];
}

/* ============================================================================
 *  SYSTEM
 * ========================================================================== */

/* Rewind snapshot: CPU core + RAM (compact, ~1.2KB per frame) */
typedef struct {
    uint8_t  A, PSW, SP, P1, P2, BUS, timer;
    uint16_t PC;
    uint8_t  flags;   /* MB,C,AC,F0,F1,BS,timer_en,counter_en */
    uint8_t  flags2;  /* timer_ovf,tcnti_en,irq_en,irq_pend,in_irq */
    int      tpre;
    uint8_t  iram[IRAM_SZ];
    uint8_t  xram[XRAM_SZ];
    float    phosphor[SW * SH];
    /* COP411L essential state */
    uint8_t  snd_ctrl_loop, snd_ctrl_vol, snd_ctrl_fast;
    uint8_t  snd_proto_state, snd_proto_hi;
    uint16_t snd_lfsr;
} RewindSnap;

/* WAV file writer state — ring buffer decouples audio thread from disk I/O */
#define WAV_RING_SZ  8192  /* must be power of 2 */
typedef struct {
    FILE *fp;
    uint32_t samples_written;
    bool active;
    /* Ring buffer: audio thread writes, main thread flushes */
    int16_t ring[WAV_RING_SZ];
    volatile uint32_t ring_wr;  /* written by audio thread */
    uint32_t ring_rd;           /* read by main thread */
} WavWriter;

struct AV {
    I8048   cpu;
    AVDisp  disp;
    COP411L snd;
    struct { bool u,d,l,r,b1,b2,b3,b4; } input;
    volatile int snd_volume;    /* 0-10, default 7 */
    uint32_t adev;              /* SDL audio device ID for thread-safe locking */
    struct { bool active, stepping; uint16_t bp[MAX_BP]; int bp_count; } dbg;
    bool running;
    bool paused;
    bool back_to_menu;
    int  frame_count;
    char save_name[128];
    /* OSD (on-screen display) */
    char osd_text[64];
    int  osd_timer;
    /* Rewind ring buffer */
    RewindSnap *rewind_buf;  /* heap-allocated [REWIND_FRAMES] */
    int  rewind_head;        /* next write position */
    int  rewind_count;       /* number of valid snapshots */
    /* WAV recording */
    WavWriter wav;
    /* Audio low-pass filter state */
    float lp_prev;
    /* Config */
    int  cfg_scale;          /* window scale factor (0=auto) */
    bool cfg_no_sound;
    /* v15: audio profile */
    int  audio_profile;      /* AUDIO_RAW/SPEAKER/HEADPHONE */
    /* v15: display settings */
    float cfg_gamma;         /* LED gamma (default 1.0) */
    float cfg_phosphor;      /* phosphor decay (default 0.45) */
    bool  scanlines;         /* scanline overlay effect */
    bool  integer_scale;     /* force integer scaling */
    bool  show_stats;        /* FPS/cycles overlay */
    bool  midframe_scan;     /* mid-frame column capture mode */
    /* v15: configurable timing */
    int   t1_pulse_start;
    int   t1_pulse_end;
    /* v15: stats tracking */
    uint32_t stat_frame_ticks; /* SDL_GetTicks at last frame */
    float stat_fps;            /* measured FPS */
    int   stat_pixels;         /* lit pixel count */
    /* v15: debugger enhancements */
    uint16_t dbg_run_to;      /* run-to-address (-1 = disabled) */
    uint16_t dbg_watch_addr;  /* XRAM watchpoint addr (-1 = none) */
    bool     dbg_watch_en;
    /* Display timing: track T1 sync for accurate column capture */
    int      disp_sync_cycle;   /* cycle when T1 went high (sync end) */
    bool     disp_sync_seen;    /* true once T1 rising edge detected */
    /* P2 tracking for hardware display emulation */
    uint8_t  prev_p2;           /* previous P2 value for edge detection */
};

/* av_led_latch: called from MOVX read (i8048_exec) to latch data to LED reg.
 * Must be defined after AV struct is fully visible. */
static void av_led_latch(AV *av, uint8_t p2, uint8_t data) {
    int ri = led_reg_decode(p2);
    if (ri >= 0) av->disp.led_reg[ri] = data;
}

static void av_port_write(AV *av, uint8_t port, uint8_t val) {
    switch (port) {
    case 0: av->cpu.BUS = val; break;
    case 1: break;
    case 2:
        /* §4.3: P2.4 rising edge = strobe LED data to display column.
         * "After all five registers have been written to, the BIOS
         * sets P2.4 high." This latches LED register contents. */
        if ((val & 0x10) && !(av->prev_p2 & 0x10)) {
            disp_latch_led_column(&av->disp);
        }
        av->prev_p2 = val;

        /* COP411L sound command protocol:
         * CPU writes $C0 to P2 → reset COP411L
         * → delays → sends command high nibble via P2 bits 4-7
         * → delays → sends command low nibble via P2 bits 4-7
         * → writes $00 to P2
         *
         * The command byte is reconstructed: (hi_nib << 4) | lo_nib
         * P2 bits 0-3 carry ROM bank address, NOT sound data.
         *
         * Lock audio device around cop411_command to prevent race
         * with the audio callback thread reading COP411L state. */
        /* Sound protocol (BIOS routine at $03A9-$03CD):
         * 1. P2=$C0 → trigger reset latch via MOVX @R0
         * 2. Wait 33 cycles → release reset via MOVX
         * 3. P2=cmd_byte → upper nibble to COP411 (bits 4-7 of P2)
         * 4. P2=SWAP(cmd_byte) → lower nibble to COP411
         * 5. P2=$00 → clear */
        if (av->snd.proto_state == 0 && val == 0xC0) {
            av->snd.proto_state = 1;
            av->snd.proto_hi = 0;
        } else if (av->snd.proto_state == 1) {
            /* Accept ANY value — the BIOS OUTL P2,A with full cmd byte */
            av->snd.proto_hi = (val >> 4) & 0x0F;
            av->snd.proto_state = 2;
        } else if (av->snd.proto_state == 2) {
            if (val == 0x00) {
#ifdef USE_SDL
                if (av->adev) SDL_LockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
                cop411_command(&av->snd, (uint8_t)(av->snd.proto_hi << 4));
#ifdef USE_SDL
                if (av->adev) SDL_UnlockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
                av->snd.proto_state = 0;
            } else {
                uint8_t lo = (val >> 4) & 0x0F;
                uint8_t cmd_byte = (uint8_t)(av->snd.proto_hi << 4) | lo;
#ifdef USE_SDL
                if (av->adev) SDL_LockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
                cop411_command(&av->snd, cmd_byte);
#ifdef USE_SDL
                if (av->adev) SDL_UnlockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
                av->snd.proto_state = 3;
            }
        } else if (av->snd.proto_state == 3) {
            if (val == 0x00) av->snd.proto_state = 0;
        }
        break;
    }
}

static uint8_t av_port_read(AV *av, uint8_t port) {
    switch (port) {
    case 0: return 0xFF;
    case 1: {
        uint8_t ext = 0xFF;
        if (av->input.b1) ext &= ~0x30;
        if (av->input.b2) ext &= ~0x50;
        if (av->input.b3) ext &= ~0x08;
        if (av->input.b4) ext &= ~0x90;
        if (av->input.u)  ext &= ~0x20;
        if (av->input.d)  ext &= ~0x10;
        if (av->input.r)  ext &= ~0x40;
        if (av->input.l)  ext &= ~0x80;
        return av->cpu.P1 & ext;
    }
    case 2: return av->cpu.P2;
    }
    return 0xFF;
}

static void av_init(AV *av) {
    memset(av, 0, sizeof(AV));
    av->running = true;
    av->snd_volume = 7;
    av->cpu.P1 = 0xFB;
    av->cpu.P2 = 0xFF;
    av->cpu.t0 = true;  /* T0 = expansion port, always 1 (MEGA doc) */
    memset(av->cpu.xram + 0x100, 0xFF, 0x300);
    cop411_init(&av->snd);
    snprintf(av->save_name, sizeof(av->save_name), "advision.sav");
    av->audio_profile = AUDIO_SPEAKER;
    av->cfg_gamma = DEF_LED_GAMMA;
    av->cfg_phosphor = DEF_PHOSPHOR;
    av->t1_pulse_start = DEF_T1_START;
    av->t1_pulse_end = DEF_T1_END;
    av->prev_p2 = 0;
    av->cpu.ei_delay = 0;
    memset(av->disp.led_reg, 0xFF, sizeof(av->disp.led_reg));
    av->disp.led_col = 0;
    av->disp.led_active = false;
    av->midframe_scan = true;  /* Default: accurate mid-frame column capture */
    av->dbg_run_to = 0xFFFF;
    av->dbg_watch_addr = 0xFFFF;
    /* Rewind buffer: allocate on first init, reuse afterwards */
    if (!av->rewind_buf)
        av->rewind_buf = (RewindSnap *)calloc(REWIND_FRAMES, sizeof(RewindSnap));
    av->rewind_head = 0;
    av->rewind_count = 0;
}

static void av_reset(AV *av) {
    uint8_t irom_bak[IROM_SZ], erom_bak[EROM_SZ];
    memcpy(irom_bak, av->cpu.irom, IROM_SZ);
    memcpy(erom_bak, av->cpu.erom, EROM_SZ);
    AVDisp disp_bak = av->disp;
    int vol = av->snd_volume;
    char sname[128]; memcpy(sname, av->save_name, 128);
    /* Preserve COP411L control register (RAM survives reset) */
    uint8_t ctrl_loop = av->snd.ctrl_loop;
    uint8_t ctrl_vol = av->snd.ctrl_vol;
    uint8_t ctrl_fast = av->snd.ctrl_fast;

    memset(&av->cpu, 0, sizeof(I8048));
    memset(&av->input, 0, sizeof(av->input));
    av->cpu.P1 = 0xFB;
    av->cpu.P2 = 0xFF;
    av->cpu.t0 = true;  /* T0 = expansion port, always 1 */
    memset(av->cpu.xram + 0x100, 0xFF, 0x300);
    memcpy(av->cpu.irom, irom_bak, IROM_SZ);
    memcpy(av->cpu.erom, erom_bak, EROM_SZ);
    av->disp = disp_bak;
    memset(av->disp.phosphor, 0, sizeof(av->disp.phosphor));
    av->snd_volume = vol;
#ifdef USE_SDL
    if (av->adev) SDL_LockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
    cop411_init(&av->snd);
    av->snd.ctrl_loop = ctrl_loop;
    av->snd.ctrl_vol = ctrl_vol;
    av->snd.ctrl_fast = ctrl_fast;
    cop411_update_ctrl_vol(&av->snd);
#ifdef USE_SDL
    if (av->adev) SDL_UnlockAudioDevice((SDL_AudioDeviceID)av->adev);
#endif
    av->frame_count = 0;
    av->paused = false;
    memcpy(av->save_name, sname, 128);
}

static void osd_show(AV *av, const char *msg) {
    snprintf(av->osd_text, sizeof(av->osd_text), "%s", msg);
    av->osd_timer = FPS * 2;
}

/* ---- Rewind ---- */
static void rewind_push(AV *av) {
    if (!av->rewind_buf) return;
    RewindSnap *s = &av->rewind_buf[av->rewind_head];
    s->A = av->cpu.A; s->PC = av->cpu.PC; s->PSW = av->cpu.PSW;
    s->SP = av->cpu.SP; s->P1 = av->cpu.P1; s->P2 = av->cpu.P2;
    s->BUS = av->cpu.BUS; s->timer = av->cpu.timer; s->tpre = av->cpu.tpre;
    s->flags = (av->cpu.MB)|(av->cpu.C<<1)|(av->cpu.AC<<2)|
              (av->cpu.F0<<3)|(av->cpu.F1<<4)|(av->cpu.BS<<5)|
              (av->cpu.timer_en<<6)|(av->cpu.counter_en<<7);
    s->flags2 = (av->cpu.timer_ovf)|(av->cpu.tcnti_en<<1)|
               (av->cpu.irq_en<<2)|(av->cpu.irq_pend<<3)|(av->cpu.in_irq<<4);
    memcpy(s->iram, av->cpu.iram, IRAM_SZ);
    memcpy(s->xram, av->cpu.xram, XRAM_SZ);
    memcpy(s->phosphor, av->disp.phosphor, sizeof(av->disp.phosphor));
    s->snd_ctrl_loop = av->snd.ctrl_loop; s->snd_ctrl_vol = av->snd.ctrl_vol;
    s->snd_ctrl_fast = av->snd.ctrl_fast; s->snd_proto_state = av->snd.proto_state;
    s->snd_proto_hi = av->snd.proto_hi; s->snd_lfsr = av->snd.lfsr;
    av->rewind_head = (av->rewind_head + 1) % REWIND_FRAMES;
    if (av->rewind_count < REWIND_FRAMES) av->rewind_count++;
}

static bool rewind_pop(AV *av) {
    if (!av->rewind_buf || av->rewind_count <= 0) return false;
    av->rewind_head = (av->rewind_head - 1 + REWIND_FRAMES) % REWIND_FRAMES;
    av->rewind_count--;
    RewindSnap *s = &av->rewind_buf[av->rewind_head];
    av->cpu.A = s->A; av->cpu.PC = s->PC; av->cpu.PSW = s->PSW;
    av->cpu.SP = s->SP; av->cpu.P1 = s->P1; av->cpu.P2 = s->P2;
    av->cpu.BUS = s->BUS; av->cpu.timer = s->timer; av->cpu.tpre = s->tpre;
    av->cpu.MB=s->flags&1; av->cpu.C=(s->flags>>1)&1; av->cpu.AC=(s->flags>>2)&1;
    av->cpu.F0=(s->flags>>3)&1; av->cpu.F1=(s->flags>>4)&1; av->cpu.BS=(s->flags>>5)&1;
    av->cpu.timer_en=(s->flags>>6)&1; av->cpu.counter_en=(s->flags>>7)&1;
    av->cpu.timer_ovf=s->flags2&1; av->cpu.tcnti_en=(s->flags2>>1)&1;
    av->prev_p2 = av->cpu.P2; av->cpu.ei_delay = 0;
    av->cpu.irq_en=(s->flags2>>2)&1; av->cpu.irq_pend=(s->flags2>>3)&1;
    av->cpu.in_irq=(s->flags2>>4)&1;
    memcpy(av->cpu.iram, s->iram, IRAM_SZ);
    memcpy(av->cpu.xram, s->xram, XRAM_SZ);
    memcpy(av->disp.phosphor, s->phosphor, sizeof(av->disp.phosphor));
    av->snd.ctrl_loop = s->snd_ctrl_loop; av->snd.ctrl_vol = s->snd_ctrl_vol;
    av->snd.ctrl_fast = s->snd_ctrl_fast; av->snd.proto_state = s->snd_proto_state;
    av->snd.proto_hi = s->snd_proto_hi; av->snd.lfsr = s->snd_lfsr;
    av->snd.active = false;
    return true;
}

/* ---- WAV recording ---- */
static void wav_start(WavWriter *w, const char *fn) {
    w->fp = fopen(fn, "wb");
    if (!w->fp) return;
    /* Write placeholder header (44 bytes), update on close */
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4); memcpy(hdr+8, "WAVEfmt ", 8);
    uint32_t v;
    v = 16; memcpy(hdr+16, &v, 4);      /* chunk size */
    uint16_t fmt = 1; memcpy(hdr+20, &fmt, 2); /* PCM */
    fmt = 1; memcpy(hdr+22, &fmt, 2);   /* mono */
    v = AUDIO_RATE; memcpy(hdr+24, &v, 4); /* sample rate */
    v = AUDIO_RATE * 2; memcpy(hdr+28, &v, 4); /* byte rate */
    fmt = 2; memcpy(hdr+32, &fmt, 2);   /* block align */
    fmt = 16; memcpy(hdr+34, &fmt, 2);  /* bits/sample */
    memcpy(hdr+36, "data", 4);
    fwrite(hdr, 44, 1, w->fp);
    w->samples_written = 0;
    w->ring_wr = 0;
    w->ring_rd = 0;
    w->active = true;
}

/* Flush ring buffer to disk — called from main thread only.
 * Writes contiguous segments in bulk for I/O efficiency. */
static void wav_flush(WavWriter *w) {
    if (!w->fp) return;
    uint32_t rd = w->ring_rd;
    uint32_t wr = w->ring_wr;  /* snapshot (audio thread is single writer) */
    if (rd == wr) return;
    /* Detect overflow: if distance > ring size, we lost samples */
    if ((wr - rd) > WAV_RING_SZ) {
        fprintf(stderr, "[WAV] Ring buffer overflow, %u samples lost\n",
                (wr - rd) - WAV_RING_SZ);
        rd = wr - WAV_RING_SZ;  /* skip to oldest available */
    }
    /* Write in up to 2 contiguous segments (ring wrap) */
    while (rd != wr) {
        uint32_t start = rd & (WAV_RING_SZ - 1);
        uint32_t end_idx = wr & (WAV_RING_SZ - 1);
        uint32_t chunk;
        if (start < end_idx || (wr - rd) >= WAV_RING_SZ)
            chunk = (wr - rd) < (WAV_RING_SZ - start) ? (wr - rd) : (WAV_RING_SZ - start);
        else
            chunk = WAV_RING_SZ - start;
        if (chunk > (wr - rd)) chunk = wr - rd;
        fwrite(&w->ring[start], sizeof(int16_t), chunk, w->fp);
        w->samples_written += chunk;
        rd += chunk;
    }
    w->ring_rd = rd;
}

static void wav_stop(WavWriter *w) {
    if (!w->fp) return;
    /* Flush any remaining samples from the ring buffer */
    wav_flush(w);
    uint32_t data_sz = w->samples_written * 2;
    uint32_t riff_sz = data_sz + 36;
    fseek(w->fp, 4, SEEK_SET); fwrite(&riff_sz, 4, 1, w->fp);
    fseek(w->fp, 40, SEEK_SET); fwrite(&data_sz, 4, 1, w->fp);
    fclose(w->fp);
    w->fp = NULL; w->active = false;
}

/* ---- Screenshot (BMP) ---- */
#ifdef USE_SDL
static void screenshot_bmp(SDL_Renderer *rr) {
    int w, h;
    SDL_GetRendererOutputSize(rr, &w, &h);
    SDL_Surface *surf = SDL_CreateRGBSurface(0, w, h, 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    if (!surf) return;
    SDL_RenderReadPixels(rr, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    /* Generate timestamped filename */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char fn[128];
    snprintf(fn, sizeof(fn), "advision_%04d%02d%02d_%02d%02d%02d.bmp",
        t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    SDL_SaveBMP(surf, fn);
    SDL_FreeSurface(surf);
    printf("Screenshot: %s\n", fn);
}
#endif

/* ---- Config file (advision.ini) ---- */
static void config_save(const AV *av, bool fullscreen) {
    FILE *f = fopen("advision.ini", "w");
    if (!f) return;
    fprintf(f, "[advision]\n");
    fprintf(f, "volume=%d\n", av->snd_volume);
    fprintf(f, "fullscreen=%d\n", fullscreen ? 1 : 0);
    fprintf(f, "scale=%d\n", av->cfg_scale);
    fprintf(f, "audio_profile=%d\n", av->audio_profile);
    fprintf(f, "gamma=%.2f\n", av->cfg_gamma);
    fprintf(f, "phosphor=%.2f\n", av->cfg_phosphor);
    fprintf(f, "scanlines=%d\n", av->scanlines ? 1 : 0);
    fprintf(f, "integer_scale=%d\n", av->integer_scale ? 1 : 0);
    fprintf(f, "# Timing (advanced)\n");
    fprintf(f, "t1_pulse_start=%d\n", av->t1_pulse_start);
    fprintf(f, "t1_pulse_end=%d\n", av->t1_pulse_end);
    fclose(f);
}

static void config_load(AV *av, bool *fullscreen) {
    FILE *f = fopen("advision.ini", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v; float fv;
        if (sscanf(line, "volume=%d", &v) == 1 && v >= 0 && v <= 10)
            av->snd_volume = v;
        if (sscanf(line, "fullscreen=%d", &v) == 1)
            *fullscreen = (v != 0);
        if (sscanf(line, "scale=%d", &v) == 1 && v >= 0 && v <= 10)
            av->cfg_scale = v;
        if (sscanf(line, "audio_profile=%d", &v) == 1 && v >= 0 && v < AUDIO_PROFILES)
            av->audio_profile = v;
        if (sscanf(line, "gamma=%f", &fv) == 1 && isfinite(fv) && fv >= 0.2f && fv <= 3.0f)
            av->cfg_gamma = fv;
        if (sscanf(line, "phosphor=%f", &fv) == 1 && isfinite(fv) && fv >= 0.0f && fv <= 1.0f)
            av->cfg_phosphor = fv;
        if (sscanf(line, "scanlines=%d", &v) == 1)
            av->scanlines = (v != 0);
        if (sscanf(line, "integer_scale=%d", &v) == 1)
            av->integer_scale = (v != 0);
        if (sscanf(line, "t1_pulse_start=%d", &v) == 1 && v >= 0 && v < 1000)
            av->t1_pulse_start = v;
        if (sscanf(line, "t1_pulse_end=%d", &v) == 1 && v >= 0 && v < 2000)
            av->t1_pulse_end = v;
        /* Validate T1 pulse: start must be < end to produce a valid pulse.
         * If inverted, the BIOS JNT1 loop never exits → infinite hang. */
        if (av->t1_pulse_start >= av->t1_pulse_end) {
            fprintf(stderr, "Warning: t1_pulse_start >= t1_pulse_end, using defaults\n");
            av->t1_pulse_start = DEF_T1_START;
            av->t1_pulse_end = DEF_T1_END;
        }
    }
    fclose(f);
}

static bool load_file(uint8_t *dest, int max_sz, const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "Cannot open '%s'\n", fn); return false; }
    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    if (file_sz < 0) { fprintf(stderr, "Cannot read size of '%s'\n", fn); fclose(f); return false; }
    if (file_sz == 0) { fprintf(stderr, "Empty file: '%s'\n", fn); fclose(f); return false; }
    if (file_sz > max_sz) {
        fprintf(stderr, "Warning: '%s' is %ld bytes, truncating to %d\n", fn, file_sz, max_sz);
        file_sz = max_sz;
    }
    fseek(f, 0, SEEK_SET);
    size_t n = fread(dest, 1, (size_t)file_sz, f);
    fclose(f);
    if ((long)n != file_sz) {
        fprintf(stderr, "Read error: expected %ld bytes, got %zu from '%s'\n", file_sz, n, fn);
        return false;
    }
    printf("Loaded %zu bytes from '%s'\n", n, fn);
    return true;
}

/* Run one frame of CPU execution with T1 mirror timing */
static void av_run_frame(AV *av) {
    int total = CYCLES_PER_FR;
    int elapsed = 0;
    av->disp_sync_seen = false;
    av->disp_sync_cycle = 0;
    /* Reset LED display state for new frame */
    memset(av->disp.led_reg, 0xFF, sizeof(av->disp.led_reg));
    av->disp.led_col = 0;
    av->disp.led_active = false;

    /* BIOS display routine timing estimate:
     * After T1 sync (rising edge), BIOS outputs 150 columns.
     * Per column: P2 setup + 5× MOVX read + P2.4 strobe ≈ 17 cycles.
     * 150 columns × 17 cycles ≈ 2550 cycles display window. */
    #define DISP_OUTPUT_CYCLES 2550

    while (elapsed < total) {
        if (av->dbg.active) {
            for (int i = 0; i < av->dbg.bp_count; i++)
                if (av->dbg.bp[i] == av->cpu.PC) { av->dbg.stepping = true; break; }
            if (av->dbg.stepping) return;
        }

        bool prev_t1 = av->cpu.t1;
        int cy = i8048_exec(&av->cpu, av);
        elapsed += cy;

        /* T1 mirror position sensor:
         * LOW pulse near start of frame to signal BIOS mirror sync.
         * The BIOS loops on JNT1 waiting for T1=0, then on T1=1 */
        bool new_t1 = !(elapsed >= av->t1_pulse_start && elapsed < av->t1_pulse_end);

        /* Detect T1 rising edge (low→high = sync pulse ended) */
        if (!prev_t1 && new_t1 && !av->disp_sync_seen) {
            av->disp_sync_cycle = elapsed;
            av->disp_sync_seen = true;
            /* Reset LED column counter: mirror reached start position.
             * BIOS will now output 150 columns via LED register + P2.4. */
            av->disp.led_col = 0;
        }

        /* Mid-frame column capture — sync-aware:
         * Columns are captured within the display output window that
         * starts immediately after T1 sync. Before sync, no columns
         * are captured (game logic is running, VRAM may be updating). */
        /* Legacy mid-frame scan: only used when LED register path inactive.
         * With LED registers, columns are captured via P2.4 strobes instead. */
        if (av->midframe_scan && !av->disp.led_active && av->disp_sync_seen) {
            int disp_elapsed = elapsed - av->disp_sync_cycle;
            if (disp_elapsed >= 0 && disp_elapsed <= DISP_OUTPUT_CYCLES) {
                int col = (disp_elapsed * SW) / DISP_OUTPUT_CYCLES;
                if (col >= 0 && col < SW)
                    disp_capture_column(&av->disp, av->cpu.xram, col);
            }
        }

        /* XRAM watchpoint check */
        if (av->dbg_watch_en && av->dbg.active) {
            /* Simple: checked after each instruction */
        }

        /* Counter mode: increment on T1 falling edge (1→0 transition).
         * MCS-48 datasheet: "Subsequent high to low transitions on T1
         * will cause the counter to increment." */
        if (av->cpu.counter_en && prev_t1 && !new_t1) {
            if (++av->cpu.timer == 0) {
                av->cpu.timer_ovf = true;
                if (av->cpu.tcnti_en && av->cpu.irq_en && !av->cpu.in_irq)
                    av->cpu.irq_pend = true;
            }
        }
        av->cpu.t1 = new_t1;
    }

    /* Column capture — hybrid strategy:
     * If LED columns were captured via P2.4 strobes (hardware-accurate),
     * they are already in col_data[]. If not (homebrew without BIOS display
     * routine), fall back to reading XRAM directly. */
    if (!av->disp.led_active && !av->midframe_scan) {
        for (int col = 0; col < SW; col++)
            disp_capture_column(&av->disp, av->cpu.xram, col);
    }

    /* Update display from captured columns */
    disp_update(&av->disp, av->cfg_phosphor);
    av->frame_count++;
    /* Push rewind snapshot every frame (lock to protect snd fields) */
#ifdef USE_SDL
    AUDIO_LOCK(av);
#endif
    rewind_push(av);
#ifdef USE_SDL
    AUDIO_UNLOCK(av);
#endif
}

/* ---- Save/Load with validation ---- */
#define SAVE_MAGIC  0x41563133  /* "AV13" */
#define SAVE_VER    18

static bool save_state(const AV *av, const char *fn) {
    FILE *f = fopen(fn, "wb");
    if (!f) { fprintf(stderr, "Cannot save to '%s'\n", fn); return false; }
    uint32_t magic = SAVE_MAGIC, ver = SAVE_VER;
    bool ok = true;
    ok = ok && fwrite(&magic, 4, 1, f) == 1;
    ok = ok && fwrite(&ver, 4, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.A, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.PC, 2, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.PSW, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.SP, 1, 1, f) == 1;
    uint8_t flags = (av->cpu.MB)|(av->cpu.C<<1)|(av->cpu.AC<<2)|
                    (av->cpu.F0<<3)|(av->cpu.F1<<4)|(av->cpu.BS<<5)|
                    (av->cpu.timer_en<<6)|(av->cpu.counter_en<<7);
    ok = ok && fwrite(&flags, 1, 1, f) == 1;
    uint8_t flags2 = (av->cpu.timer_ovf)|(av->cpu.tcnti_en<<1)|
                     (av->cpu.irq_en<<2)|(av->cpu.irq_pend<<3)|(av->cpu.in_irq<<4);
    ok = ok && fwrite(&flags2, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.timer, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.P1, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.P2, 1, 1, f) == 1;
    ok = ok && fwrite(&av->cpu.BUS, 1, 1, f) == 1;
    ok = ok && fwrite(av->cpu.iram, IRAM_SZ, 1, f) == 1;
    ok = ok && fwrite(av->cpu.xram, XRAM_SZ, 1, f) == 1;
    /* tpre as fixed 32-bit for cross-platform save compatibility */
    { uint32_t tpre32 = (uint32_t)av->cpu.tpre;
      ok = ok && fwrite(&tpre32, sizeof(uint32_t), 1, f) == 1; }
    ok = ok && fwrite(&av->cpu.cycles, sizeof(uint64_t), 1, f) == 1;
    /* COP411L state */
    ok = ok && fwrite(&av->snd.ctrl_loop, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.ctrl_vol, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.ctrl_fast, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.proto_state, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.proto_hi, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.lfsr, sizeof(uint16_t), 1, f) == 1;
    /* v15: full COP411L playback state (fixed-width for cross-platform portability) */
    { uint8_t b;
      b = av->snd.active ? 1 : 0;   ok = ok && fwrite(&b, 1, 1, f) == 1;
      b = av->snd.is_noise ? 1 : 0;  ok = ok && fwrite(&b, 1, 1, f) == 1; }
    ok = ok && fwrite(&av->snd.command, 1, 1, f) == 1;
    ok = ok && fwrite(&av->snd.cur_freq, sizeof(float), 1, f) == 1;
    ok = ok && fwrite(&av->snd.cur_vol, sizeof(float), 1, f) == 1;
    ok = ok && fwrite(&av->snd.phase_acc, sizeof(uint32_t), 1, f) == 1;
    ok = ok && fwrite(&av->snd.phase_inc, sizeof(uint32_t), 1, f) == 1;
    { int32_t i32;
      i32 = (int32_t)av->snd.cur_step;          ok = ok && fwrite(&i32, 4, 1, f) == 1;
      i32 = (int32_t)av->snd.step_count;         ok = ok && fwrite(&i32, 4, 1, f) == 1;
      i32 = (int32_t)av->snd.step_samples_left;  ok = ok && fwrite(&i32, 4, 1, f) == 1;
      i32 = (int32_t)av->snd.segment;             ok = ok && fwrite(&i32, 4, 1, f) == 1;
      i32 = (int32_t)av->snd.seg_samples_left;   ok = ok && fwrite(&i32, 4, 1, f) == 1;
      i32 = (int32_t)av->snd.seg_samples_total;  ok = ok && fwrite(&i32, 4, 1, f) == 1; }
    ok = ok && fwrite(&av->snd.seg1_vol, sizeof(float), 1, f) == 1;
    ok = ok && fwrite(&av->snd.seg2_vol, sizeof(float), 1, f) == 1;
    ok = ok && fwrite(av->snd.steps, sizeof(SndStep), MAX_SND_STEPS, f) == MAX_SND_STEPS;
    fclose(f);
    if (ok) printf("State saved.\n");
    else fprintf(stderr, "Write error saving state\n");
    return ok;
}

static bool load_state(AV *av, const char *fn) {
    FILE *f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "Cannot load '%s'\n", fn); return false; }

    uint32_t magic, ver;
    if (fread(&magic, 4, 1, f) != 1 || magic != SAVE_MAGIC) {
        fprintf(stderr, "Invalid save file (bad magic)\n"); fclose(f); return false;
    }
    if (fread(&ver, 4, 1, f) != 1 || ver != SAVE_VER) {
        fprintf(stderr, "Save version mismatch (got %u, need %u)\n", ver, SAVE_VER);
        fclose(f); return false;
    }

    /* Backup entire CPU + sound state before reading (restore on error) */
    I8048 cpu_bak = av->cpu;
    COP411L snd_bak = av->snd;

    bool ok = true;
    ok = ok && fread(&av->cpu.A, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.PC, 2, 1, f) == 1;
    ok = ok && fread(&av->cpu.PSW, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.SP, 1, 1, f) == 1;
    uint8_t flags, flags2;
    ok = ok && fread(&flags, 1, 1, f) == 1;
    ok = ok && fread(&flags2, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.timer, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.P1, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.P2, 1, 1, f) == 1;
    ok = ok && fread(&av->cpu.BUS, 1, 1, f) == 1;
    ok = ok && fread(av->cpu.iram, IRAM_SZ, 1, f) == 1;
    ok = ok && fread(av->cpu.xram, XRAM_SZ, 1, f) == 1;
    { uint32_t tpre32;
      ok = ok && fread(&tpre32, sizeof(uint32_t), 1, f) == 1;
      av->cpu.tpre = (int)tpre32; }
    ok = ok && fread(&av->cpu.cycles, sizeof(uint64_t), 1, f) == 1;
    /* COP411L state */
    ok = ok && fread(&av->snd.ctrl_loop, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.ctrl_vol, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.ctrl_fast, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.proto_state, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.proto_hi, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.lfsr, sizeof(uint16_t), 1, f) == 1;
    /* v15: full COP411L playback state (fixed-width for cross-platform portability) */
    { uint8_t b;
      ok = ok && fread(&b, 1, 1, f) == 1; av->snd.active = (b != 0);
      ok = ok && fread(&b, 1, 1, f) == 1; av->snd.is_noise = (b != 0); }
    ok = ok && fread(&av->snd.command, 1, 1, f) == 1;
    ok = ok && fread(&av->snd.cur_freq, sizeof(float), 1, f) == 1;
    ok = ok && fread(&av->snd.cur_vol, sizeof(float), 1, f) == 1;
    ok = ok && fread(&av->snd.phase_acc, sizeof(uint32_t), 1, f) == 1;
    ok = ok && fread(&av->snd.phase_inc, sizeof(uint32_t), 1, f) == 1;
    { int32_t i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.cur_step = (int)i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.step_count = (int)i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.step_samples_left = (int)i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.segment = (int)i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.seg_samples_left = (int)i32;
      ok = ok && fread(&i32, 4, 1, f) == 1; av->snd.seg_samples_total = (int)i32; }
    ok = ok && fread(&av->snd.seg1_vol, sizeof(float), 1, f) == 1;
    ok = ok && fread(&av->snd.seg2_vol, sizeof(float), 1, f) == 1;
    ok = ok && fread(av->snd.steps, sizeof(SndStep), MAX_SND_STEPS, f) == MAX_SND_STEPS;
    fclose(f);

    if (!ok) {
        fprintf(stderr, "Corrupt save file\n");
        av->cpu = cpu_bak;
        av->snd = snd_bak;  /* caller holds audio lock */
        return false;
    }

    av->cpu.MB = flags&1; av->cpu.C = (flags>>1)&1; av->cpu.AC = (flags>>2)&1;
    av->cpu.F0 = (flags>>3)&1; av->cpu.F1 = (flags>>4)&1; av->cpu.BS = (flags>>5)&1;
    av->cpu.timer_en = (flags>>6)&1; av->cpu.counter_en = (flags>>7)&1;
    av->cpu.timer_ovf = flags2&1; av->cpu.tcnti_en = (flags2>>1)&1;
    av->cpu.irq_en = (flags2>>2)&1; av->cpu.irq_pend = (flags2>>3)&1;
    av->cpu.in_irq = (flags2>>4)&1;

    av->cpu.PC &= 0xFFF;
    av->cpu.SP &= 7;
    av->cpu.t0 = true;  /* T0 = expansion port, always 1 */

    /* Preserve ROMs (not saved) */
    memcpy(av->cpu.irom, cpu_bak.irom, IROM_SZ);
    memcpy(av->cpu.erom, cpu_bak.erom, EROM_SZ);

    /* Sanitize LFSR: zero is a stuck state */
    if (av->snd.lfsr == 0) av->snd.lfsr = 0x7FFF;
    /* Sanitize loaded sound fields to valid ranges */
    av->snd.ctrl_loop &= 1;
    av->snd.ctrl_vol &= 3;
    av->snd.ctrl_fast &= 1;
    if (av->snd.proto_state > 3) av->snd.proto_state = 0;
    av->snd.proto_hi &= 0x0F;
    /* Sanitize COP411L playback fields — prevent OOB from crafted saves */
    if (av->snd.step_count < 0 || av->snd.step_count > MAX_SND_STEPS)
        av->snd.step_count = 0;
    if (av->snd.cur_step < 0 || av->snd.cur_step >= av->snd.step_count)
        av->snd.cur_step = 0;
    if (av->snd.segment < 0 || av->snd.segment > 1)
        av->snd.segment = 0;
    if (av->snd.step_samples_left < 0) av->snd.step_samples_left = 0;
    if (av->snd.seg_samples_left < 0) av->snd.seg_samples_left = 0;
    if (av->snd.seg_samples_total < 0) av->snd.seg_samples_total = 0;
    /* Reject NaN/Inf floats (isfinite returns 0 for NaN and Inf) */
    if (!isfinite(av->snd.cur_freq) || av->snd.cur_freq < 0.0f)
        av->snd.cur_freq = 0.0f;
    if (!isfinite(av->snd.cur_vol) || av->snd.cur_vol < 0.0f)
        av->snd.cur_vol = 0.0f;
    if (av->snd.cur_vol > 2.0f) av->snd.cur_vol = 1.0f;
    if (!isfinite(av->snd.seg1_vol)) av->snd.seg1_vol = 1.0f;
    if (!isfinite(av->snd.seg2_vol)) av->snd.seg2_vol = 0.5f;
    /* Sanitize step frequencies/volumes in loaded steps array */
    for (int si = 0; si < av->snd.step_count; si++) {
        SndStep *st = &av->snd.steps[si];
        if (!isfinite(st->freq) || st->freq < 0.0f) st->freq = 0.0f;
        if (!isfinite(st->volume)) st->volume = 0.0f;
        if (st->volume < 0.0f) st->volume = 0.0f;
        if (st->volume > 2.0f) st->volume = 1.0f;
        if (st->dur_ms < 0) st->dur_ms = 1;
    }

    cop411_update_ctrl_vol(&av->snd);
    /* v15: full COP411L state is now restored from savestate.
     * Caller holds audio lock (no deadlock). */
    printf("State loaded.\n");
    return true;
}

static void dbg_print(const I8048 *c) {
    printf("PC=%03X A=%02X C=%d F0=%d F1=%d BS=%d SP=%d MB=%d T=%02X P1=%02X P2=%02X\n",
           c->PC, c->A, c->C, c->F0, c->F1, c->BS, c->SP, c->MB, c->timer, c->P1, c->P2);
    printf("R0=%02X R1=%02X R2=%02X R3=%02X R4=%02X R5=%02X R6=%02X R7=%02X\n",
           c->iram[c->BS?24:0], c->iram[(c->BS?24:0)+1],
           c->iram[(c->BS?24:0)+2], c->iram[(c->BS?24:0)+3],
           c->iram[(c->BS?24:0)+4], c->iram[(c->BS?24:0)+5],
           c->iram[(c->BS?24:0)+6], c->iram[(c->BS?24:0)+7]);
}

/* ---- Built-in self-test (available in all builds) ---- */
static int run_self_test(void) {
    int pass = 0, fail = 0;
    printf("=== Adventure Vision Self-Test Suite ===\n");

    /* Test 1: CPU basics — NOP, MOV, ADD */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.irom[0] = 0x23; c.irom[1] = 0x42;  /* MOV A,#42h */
        c.irom[2] = 0x03; c.irom[3] = 0x10;  /* ADD A,#10h */
        c.irom[4] = 0x00;  /* NOP */
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        i8048_exec(&c, NULL); /* MOV A,#42h */
        if (c.A == 0x42) pass++; else { fail++; printf("FAIL: MOV A,#42h -> A=%02X\n", c.A); }
        i8048_exec(&c, NULL); /* ADD A,#10h */
        if (c.A == 0x52) pass++; else { fail++; printf("FAIL: ADD A,#10h -> A=%02X\n", c.A); }
        if (!c.C) pass++; else { fail++; printf("FAIL: carry should be 0\n"); }
    }

    /* Test 2: ADD with carry */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.irom[0] = 0x23; c.irom[1] = 0xF0; /* MOV A,#F0h */
        c.irom[2] = 0x03; c.irom[3] = 0x20; /* ADD A,#20h */
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        i8048_exec(&c, NULL);
        i8048_exec(&c, NULL);
        if (c.A == 0x10 && c.C) pass++; else { fail++; printf("FAIL: F0+20=%02X C=%d\n", c.A, c.C); }
    }

    /* Test 3: JMP */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.irom[0] = 0x04; c.irom[1] = 0x10; /* JMP $010 */
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        i8048_exec(&c, NULL);
        if (c.PC == 0x010) pass++; else { fail++; printf("FAIL: JMP -> PC=%03X\n", c.PC); }
    }

    /* Test 4: DJNZ loop */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.irom[0] = 0xB8; c.irom[1] = 0x03; /* MOV R0,#3 */
        c.irom[2] = 0xE8; c.irom[3] = 0x02; /* DJNZ R0,$02 */
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        i8048_exec(&c, NULL); /* R0=3 */
        i8048_exec(&c, NULL); /* R0=2, jump to $02 */
        i8048_exec(&c, NULL); /* R0=1, jump to $02 */
        i8048_exec(&c, NULL); /* R0=0, fall through to $04 */
        if (c.PC == 0x004 && c.iram[0] == 0) pass++;
        else { fail++; printf("FAIL: DJNZ PC=%03X R0=%02X\n", c.PC, c.iram[0]); }
    }

    /* Test 5: DAA */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.A = 0x39; c.irom[0] = 0x03; c.irom[1] = 0x28; /* ADD A,#28h */
        c.irom[2] = 0x57; /* DA A */
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        i8048_exec(&c, NULL); /* A = 0x61 */
        i8048_exec(&c, NULL); /* DAA: 0x61 -> 0x67 */
        if (c.A == 0x67) pass++; else { fail++; printf("FAIL: DAA 39+28=%02X (expected 67)\n", c.A); }
    }

    /* Test 6: Timer prescaler */
    {
        I8048 c; memset(&c, 0, sizeof(c));
        c.timer = 0xFE; c.timer_en = true;
        c.P1 = 0xFB; c.P2 = 0xFF; c.t0 = true;
        for (int i = 0; i < 100; i++) { c.irom[i] = 0x00; } /* NOPs */
        for (int i = 0; i < 64; i++) i8048_exec(&c, NULL); /* 64 cycles */
        /* Timer should have incremented twice (64/32=2): FE->FF->00 (overflow) */
        if (c.timer == 0x00 && c.timer_ovf) pass++;
        else { fail++; printf("FAIL: timer=%02X ovf=%d (expected 00,1)\n", c.timer, c.timer_ovf); }
    }

    /* Test 7: COP411L sound init */
    {
        COP411L s; cop411_init(&s);
        if (s.lfsr == 0x7FFF && !s.active) pass++;
        else { fail++; printf("FAIL: COP411L init\n"); }
    }

    /* Test 8: COP411L tone command */
    {
        COP411L s; cop411_init(&s);
        cop411_command(&s, 0xE5); /* note 5 = ~D#4 (hardware: 320.92 Hz) */
        if (s.active && !s.is_noise && s.cur_freq > 319.0f && s.cur_freq < 322.0f) pass++;
        else { fail++; printf("FAIL: tone E5 freq=%.1f active=%d\n", s.cur_freq, s.active); }
    }

    /* Test 9: COP411L noise command */
    {
        COP411L s; cop411_init(&s);
        cop411_command(&s, 0x10); /* continuous noise */
        if (s.active && s.force_loop) pass++;
        else { fail++; printf("FAIL: noise cmd\n"); }
    }

    /* Test 10: Phosphor persistence */
    {
        AVDisp d; memset(&d, 0, sizeof(d));
        d.phosphor[0] = 1.0f;
        disp_update(&d, 0.45f); /* decay */
        if (d.phosphor[0] > 0.44f && d.phosphor[0] < 0.46f) pass++;
        else { fail++; printf("FAIL: phosphor decay=%.3f\n", d.phosphor[0]); }
    }

    /* Test 11: Savestate round-trip */
    {
        AV av1, av2;
        av_init(&av1); av_init(&av2);
        av1.cpu.A = 0xAB; av1.cpu.PC = 0x123; av1.cpu.timer = 0x55;
        av1.snd.lfsr = 0x1234;
        av1.snd.active = true; av1.snd.cur_freq = 440.0f;
        save_state(&av1, "/tmp/av_test.sav");
        load_state(&av2, "/tmp/av_test.sav");
        if (av2.cpu.A == 0xAB && av2.cpu.PC == 0x123 && av2.snd.lfsr == 0x1234
            && av2.snd.active && av2.snd.cur_freq > 439.0f) pass++;
        else { fail++; printf("FAIL: savestate round-trip\n"); }
        remove("/tmp/av_test.sav");
        if (av1.rewind_buf) free(av1.rewind_buf);
        if (av2.rewind_buf) free(av2.rewind_buf);
    }

    printf("\n%d passed, %d failed (%d total)\n", pass, fail, pass+fail);
    return fail > 0 ? 1 : 0;
}

/* ---- VRAM ASCII dump ---- */
static void dump_vram_ascii(const AVDisp *d) {
    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            float v = d->phosphor[x + y * SW];
            putchar(v > 0.7f ? '#' : v > 0.3f ? '*' : v > 0.05f ? '.' : ' ');
        }
        putchar('\n');
    }
}



/* ============================================================================
 *  SDL FRONTEND + GAME SELECTOR
 * ========================================================================== */

#ifdef USE_SDL
#include <dirent.h>
#include <sys/stat.h>

/* ---- CP437-style 6x8 bitmap font (printable ASCII 32-127) ---- */
static const uint8_t font6x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x08,0x08,0x08,0x08,0x08,0x00,0x08,0x00}, /* ! */
    {0x14,0x14,0x14,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x14,0x14,0x3E,0x14,0x3E,0x14,0x14,0x00}, /* # */
    {0x08,0x1E,0x28,0x1C,0x0A,0x3C,0x08,0x00}, /* $ */
    {0x30,0x32,0x04,0x08,0x10,0x26,0x06,0x00}, /* % */
    {0x18,0x24,0x28,0x10,0x2A,0x24,0x1A,0x00}, /* & */
    {0x08,0x08,0x10,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x04,0x08,0x10,0x10,0x10,0x08,0x04,0x00}, /* ( */
    {0x10,0x08,0x04,0x04,0x04,0x08,0x10,0x00}, /* ) */
    {0x00,0x08,0x2A,0x1C,0x2A,0x08,0x00,0x00}, /* * */
    {0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x10}, /* , */
    {0x00,0x00,0x00,0x3E,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00}, /* . */
    {0x00,0x02,0x04,0x08,0x10,0x20,0x00,0x00}, /* / */
    {0x1C,0x22,0x26,0x2A,0x32,0x22,0x1C,0x00}, /* 0 */
    {0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00}, /* 1 */
    {0x1C,0x22,0x02,0x0C,0x10,0x20,0x3E,0x00}, /* 2 */
    {0x1C,0x22,0x02,0x0C,0x02,0x22,0x1C,0x00}, /* 3 */
    {0x04,0x0C,0x14,0x24,0x3E,0x04,0x04,0x00}, /* 4 */
    {0x3E,0x20,0x3C,0x02,0x02,0x22,0x1C,0x00}, /* 5 */
    {0x0C,0x10,0x20,0x3C,0x22,0x22,0x1C,0x00}, /* 6 */
    {0x3E,0x02,0x04,0x08,0x10,0x10,0x10,0x00}, /* 7 */
    {0x1C,0x22,0x22,0x1C,0x22,0x22,0x1C,0x00}, /* 8 */
    {0x1C,0x22,0x22,0x1E,0x02,0x04,0x18,0x00}, /* 9 */
    {0x00,0x00,0x08,0x00,0x00,0x08,0x00,0x00}, /* : */
    {0x00,0x00,0x08,0x00,0x00,0x08,0x08,0x10}, /* ; */
    {0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00}, /* < */
    {0x00,0x00,0x3E,0x00,0x3E,0x00,0x00,0x00}, /* = */
    {0x10,0x08,0x04,0x02,0x04,0x08,0x10,0x00}, /* > */
    {0x1C,0x22,0x02,0x04,0x08,0x00,0x08,0x00}, /* ? */
    {0x1C,0x22,0x2E,0x2A,0x2E,0x20,0x1C,0x00}, /* @ */
    {0x1C,0x22,0x22,0x3E,0x22,0x22,0x22,0x00}, /* A */
    {0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0x00}, /* B */
    {0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0x00}, /* C */
    {0x38,0x24,0x22,0x22,0x22,0x24,0x38,0x00}, /* D */
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0x00}, /* E */
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0x00}, /* F */
    {0x1C,0x22,0x20,0x2E,0x22,0x22,0x1E,0x00}, /* G */
    {0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0x00}, /* H */
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00}, /* I */
    {0x0E,0x04,0x04,0x04,0x04,0x24,0x18,0x00}, /* J */
    {0x22,0x24,0x28,0x30,0x28,0x24,0x22,0x00}, /* K */
    {0x20,0x20,0x20,0x20,0x20,0x20,0x3E,0x00}, /* L */
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0x00}, /* M */
    {0x22,0x32,0x2A,0x26,0x22,0x22,0x22,0x00}, /* N */
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00}, /* O */
    {0x3C,0x22,0x22,0x3C,0x20,0x20,0x20,0x00}, /* P */
    {0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0x00}, /* Q */
    {0x3C,0x22,0x22,0x3C,0x28,0x24,0x22,0x00}, /* R */
    {0x1C,0x22,0x20,0x1C,0x02,0x22,0x1C,0x00}, /* S */
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, /* T */
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0x00}, /* U */
    {0x22,0x22,0x22,0x22,0x14,0x14,0x08,0x00}, /* V */
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0x00}, /* W */
    {0x22,0x22,0x14,0x08,0x14,0x22,0x22,0x00}, /* X */
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08,0x00}, /* Y */
    {0x3E,0x02,0x04,0x08,0x10,0x20,0x3E,0x00}, /* Z */
    {0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0x00}, /* [ */
    {0x00,0x20,0x10,0x08,0x04,0x02,0x00,0x00}, /* \ */
    {0x1C,0x04,0x04,0x04,0x04,0x04,0x1C,0x00}, /* ] */
    {0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x3E,0x00}, /* _ */
    {0x10,0x08,0x04,0x00,0x00,0x00,0x00,0x00}, /* ` */
    {0x00,0x00,0x1C,0x02,0x1E,0x22,0x1E,0x00}, /* a */
    {0x20,0x20,0x3C,0x22,0x22,0x22,0x3C,0x00}, /* b */
    {0x00,0x00,0x1C,0x20,0x20,0x20,0x1C,0x00}, /* c */
    {0x02,0x02,0x1E,0x22,0x22,0x22,0x1E,0x00}, /* d */
    {0x00,0x00,0x1C,0x22,0x3E,0x20,0x1C,0x00}, /* e */
    {0x0C,0x12,0x10,0x3C,0x10,0x10,0x10,0x00}, /* f */
    {0x00,0x00,0x1E,0x22,0x1E,0x02,0x1C,0x00}, /* g */
    {0x20,0x20,0x2C,0x32,0x22,0x22,0x22,0x00}, /* h */
    {0x08,0x00,0x18,0x08,0x08,0x08,0x1C,0x00}, /* i */
    {0x04,0x00,0x04,0x04,0x04,0x24,0x18,0x00}, /* j */
    {0x20,0x20,0x24,0x28,0x30,0x28,0x24,0x00}, /* k */
    {0x18,0x08,0x08,0x08,0x08,0x08,0x1C,0x00}, /* l */
    {0x00,0x00,0x34,0x2A,0x2A,0x2A,0x2A,0x00}, /* m */
    {0x00,0x00,0x2C,0x32,0x22,0x22,0x22,0x00}, /* n */
    {0x00,0x00,0x1C,0x22,0x22,0x22,0x1C,0x00}, /* o */
    {0x00,0x00,0x3C,0x22,0x3C,0x20,0x20,0x00}, /* p */
    {0x00,0x00,0x1E,0x22,0x1E,0x02,0x02,0x00}, /* q */
    {0x00,0x00,0x2C,0x32,0x20,0x20,0x20,0x00}, /* r */
    {0x00,0x00,0x1E,0x20,0x1C,0x02,0x3C,0x00}, /* s */
    {0x10,0x10,0x3C,0x10,0x10,0x12,0x0C,0x00}, /* t */
    {0x00,0x00,0x22,0x22,0x22,0x26,0x1A,0x00}, /* u */
    {0x00,0x00,0x22,0x22,0x22,0x14,0x08,0x00}, /* v */
    {0x00,0x00,0x22,0x22,0x2A,0x2A,0x14,0x00}, /* w */
    {0x00,0x00,0x22,0x14,0x08,0x14,0x22,0x00}, /* x */
    {0x00,0x00,0x22,0x22,0x1E,0x02,0x1C,0x00}, /* y */
    {0x00,0x00,0x3E,0x04,0x08,0x10,0x3E,0x00}, /* z */
    {0x0C,0x10,0x10,0x20,0x10,0x10,0x0C,0x00}, /* { */
    {0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00}, /* | */
    {0x18,0x04,0x04,0x02,0x04,0x04,0x18,0x00}, /* } */
    {0x00,0x00,0x10,0x2A,0x04,0x00,0x00,0x00}, /* ~ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* DEL */
};

static void draw_char(SDL_Renderer *rr, int x, int y, char ch, int sc,
                      uint8_t cr, uint8_t cg, uint8_t cb) {
    int idx = (unsigned char)ch - 32;
    if (idx < 0 || idx >= 96) return;
    SDL_SetRenderDrawColor(rr, cr, cg, cb, 255);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = font6x8[idx][row];
        for (int col = 0; col < 6; col++)
            if (bits & (0x20 >> col)) {
                SDL_Rect rc = { x+col*sc, y+row*sc, sc, sc };
                SDL_RenderFillRect(rr, &rc);
            }
    }
}

static void draw_text(SDL_Renderer *rr, int x, int y, const char *s, int sc,
                      uint8_t cr, uint8_t cg, uint8_t cb) {
    for (; *s; s++, x += 7*sc)
        draw_char(rr, x, y, *s, sc, cr, cg, cb);
}

static int text_width(const char *s, int sc) { return (int)strlen(s) * 7 * sc; }

/* ---- ROM scanner ---- */
#define MAX_ROMS 64
#define PATH_MAX_LEN 512

typedef struct {
    char path[PATH_MAX_LEN];
    char name[128];
    long size;
} RomEntry;

static int scan_roms(const char *dir, RomEntry *out, int max) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < max) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 3 || len >= 128) continue;
        bool ext_ok = false;
        if (len >= 4) {
            const char *ext = name + len - 4;
            ext_ok = (strcasecmp(ext, ".bin") == 0 || strcasecmp(ext, ".rom") == 0);
        }
        {
            const char *ext3 = name + len - 3;
            if (strcasecmp(ext3, ".u5") == 0 || strcasecmp(ext3, ".u1") == 0 ||
                strcasecmp(ext3, ".u2") == 0 || strcasecmp(ext3, ".u3") == 0 ||
                strcasecmp(ext3, ".u4") == 0 || strcasecmp(ext3, ".u6") == 0)
                ext_ok = true;
        }
        if (!ext_ok) continue;
        snprintf(out[n].path, PATH_MAX_LEN, "%s/%s", dir, name);
        struct stat st;
        if (stat(out[n].path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out[n].size = (long)st.st_size;
        snprintf(out[n].name, 128, "%s", name);
        printf("[SCAN] Found: %s (%ld bytes)\n", name, out[n].size);
        n++;
    }
    closedir(d);
    return n;
}

/* ---- Game selector ---- */
#define MAX_GAMES 16

typedef struct {
    char bios_path[PATH_MAX_LEN];
    char game_paths[MAX_GAMES][PATH_MAX_LEN];
    char game_names[MAX_GAMES][128];
    int  game_embed_idx[MAX_GAMES];
    int  game_count;
    int  selected;
    bool has_bios;
    bool bios_embedded;
} GameMenu;

static const struct { const char *pat; const char *title; } known_games[] = {
    {"defender","Defender"},{"turtles","Turtles"},
    {"super_cobra","Super Cobra"},{"supercobra","Super Cobra"},
    {"super cobra","Super Cobra"},{"space_force","Space Force"},
    {"spaceforce","Space Force"},{"space force","Space Force"},
    {NULL,NULL}
};

/* ---- Game info database for the info panel ---- */
typedef struct {
    const char *name;       /* matches prettified game name */
    const char *year;
    const char *developer;
    const char *genre;
    const char *desc[5];    /* description lines (NULL-terminated) */
    const char *controls;   /* per-game control hint */
} GameInfo;

static const GameInfo game_db[] = {
    { "Defender", "1982", "Entex / Williams",
      "Horizontal shoot'em up",
      { "Port of the classic Williams",
        "arcade game. Protect humanoids",
        "from waves of alien abductors",
        "across a scrolling landscape.",
        NULL },
      "Z:fire X:thrust A:smart bomb"
    },
    { "Super Cobra", "1982", "Entex / Konami",
      "Horizontal shoot'em up",
      { "Fly a helicopter through enemy",
        "territory, dodging missiles and",
        "obstacles. Destroy fuel tanks",
        "to keep flying. 10 stages.",
        NULL },
      "Z:fire X:bomb"
    },
    { "Space Force", "1982", "Entex",
      "Fixed-screen shooter",
      { "Original Entex title. Defend",
        "your base against descending",
        "waves of alien invaders in",
        "this fast-paced space shooter.",
        NULL },
      "Z:fire"
    },
    { "Turtles", "1982", "Entex / Stern / Konami",
      "Maze / rescue",
      { "Guide baby turtles through a",
        "maze back to their home while",
        "avoiding beetles. Port of the",
        "Stern arcade original.",
        NULL },
      "Arrows:move Z:mystery box"
    },
    { "Table Tennis", "2020", "Ben Larson (homebrew)",
      "Sports / Pong",
      { "Homebrew table tennis / Pong",
        "for the Adventure Vision's",
        "unique LED display.",
        NULL, NULL },
      "Up/Down:paddle Z:serve"
    },
    { NULL, NULL, NULL, NULL, {NULL,NULL,NULL,NULL,NULL}, NULL }
};

static const GameInfo *find_game_info(const char *name) {
    for (int i = 0; game_db[i].name; i++) {
        if (strcasestr(name, game_db[i].name))
            return &game_db[i];
    }
    return NULL;
}

/* ---- Procedural cover art for known games ---- */
static void draw_cover_defender(SDL_Renderer *rr, int x, int y, int w, int h) {
    /* Starfield */
    SDL_SetRenderDrawColor(rr, 4, 2, 8, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    SDL_SetRenderDrawColor(rr, 100, 80, 60, 255);
    for (int i = 0; i < 20; i++) {
        int sx = x + (i * 37 + 13) % w;
        int sy = y + (i * 23 + 7) % (h - 30);
        SDL_Rect st = {sx, sy, 1, 1};
        SDL_RenderFillRect(rr, &st);
    }
    /* Terrain */
    SDL_SetRenderDrawColor(rr, 100, 45, 15, 255);
    int th = h / 5;
    for (int tx = 0; tx < w; tx++) {
        int ty = th + (int)(6.0f * sinf((float)tx * 0.08f) + 3.0f * sinf((float)tx * 0.2f));
        SDL_Rect t = {x + tx, y + h - ty, 1, ty};
        SDL_RenderFillRect(rr, &t);
    }
    /* Ship */
    int sx = x + w / 3, sy = y + h / 2 - 5;
    SDL_SetRenderDrawColor(rr, 220, 60, 20, 255);
    SDL_Rect sh1 = {sx, sy + 2, 14, 4};
    SDL_Rect sh2 = {sx + 14, sy + 3, 4, 2};
    SDL_Rect sh3 = {sx - 2, sy, 4, 8};
    SDL_RenderFillRect(rr, &sh1);
    SDL_RenderFillRect(rr, &sh2);
    SDL_RenderFillRect(rr, &sh3);
    /* Title */
    draw_text(rr, x + (w - text_width("DEFENDER", 1)) / 2, y + 4, "DEFENDER", 1, 200, 50, 20);
}

static void draw_cover_cobra(SDL_Renderer *rr, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(rr, 4, 6, 4, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    /* Mountains */
    for (int tx = 0; tx < w; tx++) {
        float m = 20.0f * sinf((float)tx * 0.04f) + 10.0f * sinf((float)tx * 0.11f + 1.0f);
        int mh = (int)m + h / 3;
        if (mh < 5) mh = 5;
        SDL_SetRenderDrawColor(rr, 50, 70, 35, 255);
        SDL_Rect mt = {x + tx, y + h - mh, 1, mh};
        SDL_RenderFillRect(rr, &mt);
    }
    /* Helicopter body */
    int hx = x + w / 3, hy = y + h / 3;
    SDL_SetRenderDrawColor(rr, 200, 55, 20, 255);
    SDL_Rect b1 = {hx, hy, 12, 6};
    SDL_Rect b2 = {hx + 12, hy + 1, 4, 4};
    SDL_Rect b3 = {hx - 6, hy + 2, 6, 2};  /* tail */
    SDL_Rect b4 = {hx + 2, hy - 2, 10, 1}; /* rotor */
    SDL_RenderFillRect(rr, &b1);
    SDL_RenderFillRect(rr, &b2);
    SDL_RenderFillRect(rr, &b3);
    SDL_RenderFillRect(rr, &b4);
    /* Fuel tanks */
    SDL_SetRenderDrawColor(rr, 160, 130, 30, 255);
    for (int i = 0; i < 3; i++) {
        SDL_Rect ft = {x + w/2 + i*30, y + h - 18, 6, 8};
        SDL_RenderFillRect(rr, &ft);
    }
    draw_text(rr, x + (w - text_width("SUPER COBRA", 1)) / 2, y + 4, "SUPER COBRA", 1, 200, 50, 20);
}

static void draw_cover_space(SDL_Renderer *rr, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(rr, 2, 2, 10, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    /* Stars */
    SDL_SetRenderDrawColor(rr, 120, 100, 80, 255);
    for (int i = 0; i < 30; i++) {
        SDL_Rect st = {x + (i*41+5) % w, y + (i*29+3) % h, 1, 1};
        SDL_RenderFillRect(rr, &st);
    }
    /* Enemy formation */
    SDL_SetRenderDrawColor(rr, 180, 50, 20, 255);
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 5; col++) {
            SDL_Rect en = {x + w/4 + col*20, y + 20 + row*14, 8, 6};
            SDL_RenderFillRect(rr, &en);
        }
    /* Player */
    SDL_SetRenderDrawColor(rr, 220, 70, 25, 255);
    int px = x + w / 2 - 4;
    SDL_Rect p1 = {px, y + h - 20, 8, 6};
    SDL_Rect p2 = {px + 3, y + h - 24, 2, 4};
    SDL_RenderFillRect(rr, &p1);
    SDL_RenderFillRect(rr, &p2);
    draw_text(rr, x + (w - text_width("SPACE FORCE", 1)) / 2, y + 4, "SPACE FORCE", 1, 200, 50, 20);
}

static void draw_cover_turtles(SDL_Renderer *rr, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(rr, 4, 4, 2, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    /* Maze grid */
    SDL_SetRenderDrawColor(rr, 80, 50, 25, 255);
    for (int gx = 0; gx < 6; gx++) {
        SDL_Rect vl = {x + 15 + gx * 25, y + 18, 1, h - 36};
        SDL_RenderFillRect(rr, &vl);
    }
    for (int gy = 0; gy < 5; gy++) {
        SDL_Rect hl = {x + 15, y + 18 + gy * 20, w - 30, 1};
        SDL_RenderFillRect(rr, &hl);
    }
    /* Turtles */
    SDL_SetRenderDrawColor(rr, 50, 180, 50, 255);
    int tx[] = {30, 80, 55};
    int ty[] = {40, 60, 80};
    for (int i = 0; i < 3; i++) {
        SDL_Rect tb = {x + tx[i], y + ty[i], 6, 5};
        SDL_RenderFillRect(rr, &tb);
    }
    /* Beetle */
    SDL_SetRenderDrawColor(rr, 180, 40, 20, 255);
    SDL_Rect bt = {x + 100, y + 50, 5, 5};
    SDL_RenderFillRect(rr, &bt);
    draw_text(rr, x + (w - text_width("TURTLES", 1)) / 2, y + 4, "TURTLES", 1, 200, 50, 20);
}

static void draw_cover_tennis(SDL_Renderer *rr, int x, int y, int w, int h) {
    SDL_SetRenderDrawColor(rr, 4, 4, 4, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    /* Net */
    SDL_SetRenderDrawColor(rr, 60, 40, 25, 255);
    for (int ny = 0; ny < h - 20; ny += 4) {
        SDL_Rect n = {x + w/2, y + 10 + ny, 1, 2};
        SDL_RenderFillRect(rr, &n);
    }
    /* Paddles */
    SDL_SetRenderDrawColor(rr, 200, 55, 20, 255);
    SDL_Rect p1 = {x + 15, y + h/2 - 10, 4, 20};
    SDL_Rect p2 = {x + w - 19, y + h/2 - 8, 4, 20};
    SDL_RenderFillRect(rr, &p1);
    SDL_RenderFillRect(rr, &p2);
    /* Ball */
    SDL_SetRenderDrawColor(rr, 220, 180, 40, 255);
    SDL_Rect ball = {x + w/2 + 15, y + h/2 - 2, 4, 4};
    SDL_RenderFillRect(rr, &ball);
    draw_text(rr, x + (w - text_width("TABLE TENNIS", 1)) / 2, y + 4, "TABLE TENNIS", 1, 200, 50, 20);
}

static void draw_cover_generic(SDL_Renderer *rr, int x, int y, int w, int h, const char *name) {
    SDL_SetRenderDrawColor(rr, 8, 4, 4, 255);
    SDL_Rect bg = {x, y, w, h};
    SDL_RenderFillRect(rr, &bg);
    /* AV logo outline */
    SDL_SetRenderDrawColor(rr, 80, 30, 15, 255);
    SDL_Rect brd = {x + 4, y + 4, w - 8, h - 8};
    SDL_RenderDrawRect(rr, &brd);
    /* Game name centered */
    int tw = text_width(name, 1);
    if (tw > w - 10) tw = w - 10;
    draw_text(rr, x + (w - tw) / 2, y + h / 2 - 4, name, 1, 160, 60, 30);
}

static void draw_cover(SDL_Renderer *rr, int x, int y, int w, int h, const char *name) {
    if (strcasestr(name, "Defender"))         draw_cover_defender(rr, x, y, w, h);
    else if (strcasestr(name, "Super Cobra")) draw_cover_cobra(rr, x, y, w, h);
    else if (strcasestr(name, "Space Force")) draw_cover_space(rr, x, y, w, h);
    else if (strcasestr(name, "Turtles"))     draw_cover_turtles(rr, x, y, w, h);
    else if (strcasestr(name, "Table Tennis"))draw_cover_tennis(rr, x, y, w, h);
    else                                      draw_cover_generic(rr, x, y, w, h, name);
    /* Border */
    SDL_SetRenderDrawColor(rr, 100, 40, 20, 255);
    SDL_Rect brd = {x, y, w, h};
    SDL_RenderDrawRect(rr, &brd);
}

static const char *prettify_name(const char *filename) {
    static char buf[128];
    for (int i = 0; known_games[i].pat; i++)
        if (strcasestr(filename, known_games[i].pat))
            return known_games[i].title;
    snprintf(buf, sizeof(buf), "%s", filename);
    char *dot = strrchr(buf, '.');
    if (dot) *dot = '\0';
    for (char *p = buf; *p; p++) { if (*p == '_') *p = ' '; if (*p == '-') *p = ' '; }
    if (buf[0] >= 'a' && buf[0] <= 'z') buf[0] -= 32;
    return buf;
}

static bool is_bios(const RomEntry *r) {
    if (r->size == 1024) return true;  /* exact BIOS size */
    if (strcasestr(r->name, "bios")) return true;
    if (strcasestr(r->name, "ins8048")) return true;
    if (strcasestr(r->name, "b225")) return true;
    if (strcasestr(r->name, ".u5")) return true;
    return false;
}

static bool is_game(const RomEntry *r) {
    if (is_bios(r)) return false;
    return r->size >= 512 && r->size <= 8192;
}

static void menu_scan(GameMenu *m, const char *dir) {
    m->game_count = 0;
    m->has_bios = false;
    m->bios_embedded = false;
    m->selected = 0;

#ifdef EMBED_ROMS
    m->has_bios = true;
    m->bios_embedded = true;
    printf("[MENU] BIOS: embedded (%d bytes)\n", (int)sizeof(embedded_bios));
    for (int i = 0; i < EMBEDDED_GAME_COUNT && m->game_count < MAX_GAMES; i++) {
        m->game_embed_idx[m->game_count] = i;
        m->game_paths[m->game_count][0] = '\0';
        snprintf(m->game_names[m->game_count], 128, "%s", embedded_games[i].name);
        printf("[MENU] Game: embedded[%d] \"%s\" (%d bytes)\n",
               i, embedded_games[i].name, embedded_games[i].size);
        m->game_count++;
    }
#endif

    RomEntry roms[MAX_ROMS];
    int n = scan_roms(dir, roms, MAX_ROMS);

    if (!m->has_bios) {
        for (int i = 0; i < n; i++) {
            if (is_bios(&roms[i])) {
                snprintf(m->bios_path, PATH_MAX_LEN, "%s", roms[i].path);
                m->has_bios = true;
                printf("[MENU] BIOS: %s (%ld bytes)\n", roms[i].name, roms[i].size);
                break;
            }
        }
    }
    for (int i = 0; i < n && m->game_count < MAX_GAMES; i++) {
        if (is_game(&roms[i])) {
            const char *pretty = prettify_name(roms[i].name);
            /* Skip if a game with a matching name is already listed (e.g. embedded).
             * Use substring match: "Super Cobra" matches "Super Cobra (USA, Europe)" */
            bool dup = false;
            for (int j = 0; j < m->game_count; j++) {
                if (strcasestr(m->game_names[j], pretty) ||
                    strcasestr(pretty, m->game_names[j])) { dup = true; break; }
            }
            if (dup) {
                printf("[MENU] Skip duplicate: %s\n", roms[i].name);
                continue;
            }
            m->game_embed_idx[m->game_count] = -1;
            snprintf(m->game_paths[m->game_count], PATH_MAX_LEN, "%s", roms[i].path);
            snprintf(m->game_names[m->game_count], 128, "%s", pretty);
            printf("[MENU] Game: %s (%ld bytes) -> \"%s\"\n",
                   roms[i].name, roms[i].size, m->game_names[m->game_count]);
            m->game_count++;
        }
    }

    /* Sort games alphabetically (simple bubble sort, max 16 games) */
    for (int i = 0; i < m->game_count - 1; i++) {
        for (int j = i + 1; j < m->game_count; j++) {
            if (strcasecmp(m->game_names[i], m->game_names[j]) > 0) {
                char tmp_path[PATH_MAX_LEN], tmp_name[128];
                int tmp_idx;
                memcpy(tmp_path, m->game_paths[i], PATH_MAX_LEN);
                memcpy(tmp_name, m->game_names[i], 128);
                tmp_idx = m->game_embed_idx[i];
                memcpy(m->game_paths[i], m->game_paths[j], PATH_MAX_LEN);
                memcpy(m->game_names[i], m->game_names[j], 128);
                m->game_embed_idx[i] = m->game_embed_idx[j];
                memcpy(m->game_paths[j], tmp_path, PATH_MAX_LEN);
                memcpy(m->game_names[j], tmp_name, 128);
                m->game_embed_idx[j] = tmp_idx;
            }
        }
    }
}

/* ---- Cover texture from embedded pixel data ---- */
#ifdef EMBED_COVERS
typedef struct { const char *name; const uint32_t *data; } CoverEntry;
static const CoverEntry cover_entries[] = {
    { "Defender",     cover_defender },
    { "Super Cobra",  cover_super_cobra },
    { "Space Force",  cover_space_force },
    { "Turtles",      cover_turtles },
    { NULL, NULL }
};

static SDL_Texture *create_cover_texture(SDL_Renderer *rr, const uint32_t *argb) {
    /* Enable bilinear filtering for this texture */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Texture *tex = SDL_CreateTexture(rr, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC, COVER_THUMB_W, COVER_THUMB_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); /* restore nearest for pixel art */
    if (!tex) return NULL;
    SDL_UpdateTexture(tex, NULL, argb, COVER_THUMB_W * sizeof(uint32_t));
    return tex;
}

static const uint32_t *find_cover_data(const char *name) {
    for (int i = 0; cover_entries[i].name; i++)
        if (strcasestr(name, cover_entries[i].name))
            return cover_entries[i].data;
    return NULL;
}
#endif





static int menu_run(GameMenu *m, SDL_Renderer *rr, SDL_Window *win) {
    const int LIST_X = 20, LIST_W = 300;
    const int LIST_Y0 = 68, LIST_ROW_H = 18;
    const int PANEL_X = 340;
    const int COVER_X = PANEL_X + 8, COVER_Y = 56;
    const int COVER_W = 126, COVER_H = 180;
    const int TEXT_X  = COVER_X + COVER_W + 14;

    Uint32 last_click_time = 0;
    int last_click_idx = -1;

#ifdef EMBED_COVERS
    SDL_Texture *cover_tex[MAX_GAMES];
    memset(cover_tex, 0, sizeof(cover_tex));
    for (int i = 0; i < m->game_count; i++) {
        const uint32_t *data = find_cover_data(m->game_names[i]);
        if (data) cover_tex[i] = create_cover_texture(rr, data);
    }
#endif

    SDL_RenderSetLogicalSize(rr, 0, 0);

    /* Render target at full output resolution — recreated on resize.
     * Bilinear filtering so scaling to screen looks smooth. */
    int rt_ow = 0, rt_oh = 0;
    SDL_Texture *rt = NULL;

    while (1) {
        /* Check output size, (re)create render target if needed */
        int ow, oh;
        SDL_GetRendererOutputSize(rr, &ow, &oh);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        if (ow != rt_ow || oh != rt_oh) {
            if (rt) SDL_DestroyTexture(rt);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
            rt = SDL_CreateTexture(rr, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET, ow, oh);
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
            rt_ow = ow;
            rt_oh = oh;
        }

        /* Compute scale to fit MENU_LW×MENU_LH into the output */
        float osx = (float)ow / (float)MENU_LW;
        float osy = (float)oh / (float)MENU_LH;
        float osc = (osx < osy) ? osx : osy;

        /* Window-space letterbox rect (mouse events live here) */
        int ww, wh;
        SDL_GetWindowSize(win, &ww, &wh);
        if (ww < 1) ww = 1;
        if (wh < 1) wh = 1;
        float wsx = (float)ww / (float)MENU_LW;
        float wsy = (float)wh / (float)MENU_LH;
        float wsc = (wsx < wsy) ? wsx : wsy;
        int dw = (int)((float)MENU_LW * wsc);
        int dh = (int)((float)MENU_LH * wsc);
        int dx = (ww - dw) / 2;
        int dy = (wh - dh) / 2;

        /* Output-space blit destination (may differ from window on HiDPI) */
        float dpix = (float)ow / (float)ww;
        float dpiy = (float)oh / (float)wh;
        SDL_Rect blit_dst = {
            (int)((float)dx * dpix), (int)((float)dy * dpiy),
            (int)((float)dw * dpix), (int)((float)dh * dpiy)
        };

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) goto menu_exit_quit;

            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_UP:
                    if (m->game_count > 0)
                        m->selected = (m->selected - 1 + m->game_count) % m->game_count;
                    break;
                case SDLK_DOWN:
                    if (m->game_count > 0)
                        m->selected = (m->selected + 1) % m->game_count;
                    break;
                case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_z:
                    if (m->has_bios && m->game_count > 0) goto menu_exit_play;
                    break;
                case SDLK_ESCAPE: goto menu_exit_quit;
                default: break;
                }
            }

            /* Mouse: window coords → logical via window-space letterbox.
             * Both mouse events and dx/dy/dw/dh are in window pixel space. */
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int mx = (dw > 0) ? (e.button.x - dx) * MENU_LW / dw : 0;
                int my = (dh > 0) ? (e.button.y - dy) * MENU_LH / dh : 0;

                if (mx >= LIST_X - 5 && mx < LIST_X + LIST_W + 5 &&
                    my >= LIST_Y0 && m->game_count > 0) {
                    int idx = (my - LIST_Y0) / LIST_ROW_H;
                    if (idx >= 0 && idx < m->game_count) {
                        Uint32 now = SDL_GetTicks();
                        if (idx == last_click_idx && (now - last_click_time) < 500) {
                            if (m->has_bios) { m->selected = idx; goto menu_exit_play; }
                        }
                        m->selected = idx;
                        last_click_idx = idx;
                        last_click_time = now;
                    }
                }
            }
        }

        /* ---- Draw into high-res render target ---- */
        SDL_SetRenderTarget(rr, rt);
        SDL_RenderSetViewport(rr, NULL);
        SDL_RenderSetScale(rr, osc, osc);

        SDL_SetRenderDrawColor(rr, 10, 4, 4, 255);
        SDL_RenderClear(rr);

        /* Top accent */
        SDL_SetRenderDrawColor(rr, 160, 35, 15, 255);
        { SDL_Rect r = { 0, 0, MENU_LW, 2 }; SDL_RenderFillRect(rr, &r); }

        {
            const char *title = "ADVENTURE VISION";
            int tw = text_width(title, 2);
            draw_text(rr, (MENU_LW - tw) / 2, 8, title, 2, 200, 50, 20);
        }
        draw_text(rr, 20, 30, "Entex 1982 Emulator v15", 1, 100, 70, 55);

        SDL_SetRenderDrawColor(rr, 60, 15, 10, 255);
        { SDL_Rect r = { 20, 44, MENU_LW - 40, 1 }; SDL_RenderFillRect(rr, &r); }

        int ypos = 50;
        if (!m->has_bios) {
            draw_text(rr, LIST_X, ypos, "! BIOS not found", 1, 255, 90, 70);
            ypos += 10;
            draw_text(rr, LIST_X, ypos, "  Place 1KB BIOS in this folder", 1, 150, 100, 80);
            ypos += 14;
        }
        if (m->game_count == 0) {
            draw_text(rr, LIST_X, ypos, "! No game ROMs found", 1, 255, 90, 70);
            ypos += 10;
            draw_text(rr, LIST_X, ypos, "  Place .bin/.rom files here", 1, 150, 100, 80);
        }

        if (m->game_count > 0) {
            draw_text(rr, LIST_X, 52, "Select game:", 1, 120, 100, 85);

            for (int i = 0; i < m->game_count; i++) {
                int gy = LIST_Y0 + i * LIST_ROW_H;
                if (gy + 14 > MENU_LH - 25) break;
                if (i == m->selected) {
                    SDL_SetRenderDrawColor(rr, 40, 10, 8, 255);
                    { SDL_Rect r = { LIST_X - 2, gy, LIST_W, LIST_ROW_H - 2 }; SDL_RenderFillRect(rr, &r); }
                    SDL_SetRenderDrawColor(rr, 200, 50, 18, 255);
                    { SDL_Rect r = { LIST_X, gy + 2, 2, 12 }; SDL_RenderFillRect(rr, &r); }
                    draw_text(rr, LIST_X + 10, gy + 3, m->game_names[i], 1, 255, 230, 210);
                } else {
                    draw_text(rr, LIST_X + 10, gy + 3, m->game_names[i], 1, 130, 95, 75);
                }
            }
        }

        SDL_SetRenderDrawColor(rr, 40, 15, 10, 255);
        { SDL_Rect r = { PANEL_X - 10, 48, 1, MENU_LH - 75 }; SDL_RenderFillRect(rr, &r); }

        if (m->game_count > 0 && m->selected >= 0 && m->selected < m->game_count) {
            const char *gname = m->game_names[m->selected];

            bool drew_photo = false;
#ifdef EMBED_COVERS
            if (m->selected < MAX_GAMES && cover_tex[m->selected]) {
                SDL_Rect dst = { COVER_X, COVER_Y, COVER_W, COVER_H };
                SDL_RenderCopy(rr, cover_tex[m->selected], NULL, &dst);
                drew_photo = true;
            }
#endif
            if (!drew_photo)
                draw_cover(rr, COVER_X, COVER_Y, COVER_W, COVER_H, gname);

            SDL_SetRenderDrawColor(rr, 80, 35, 18, 255);
            { SDL_Rect r = {COVER_X-1, COVER_Y-1, COVER_W+2, COVER_H+2}; SDL_RenderDrawRect(rr, &r); }

            const GameInfo *gi = find_game_info(gname);
            int iy = COVER_Y + 2;
            draw_text(rr, TEXT_X, iy, gname, 1, 220, 180, 150);
            iy += 16;

            if (gi) {
                draw_text(rr, TEXT_X, iy, gi->year, 1, 130, 95, 70);
                iy += 12;
                draw_text(rr, TEXT_X, iy, gi->developer, 1, 120, 85, 65);
                iy += 14;
                SDL_SetRenderDrawColor(rr, 50, 18, 10, 255);
                int gtw = text_width(gi->genre, 1);
                { SDL_Rect r = {TEXT_X-2, iy-1, gtw+4, 11}; SDL_RenderFillRect(rr, &r); }
                draw_text(rr, TEXT_X, iy, gi->genre, 1, 180, 70, 35);
                iy += 18;
                SDL_SetRenderDrawColor(rr, 45, 18, 10, 255);
                { SDL_Rect r = {TEXT_X, iy, 180, 1}; SDL_RenderFillRect(rr, &r); }
                iy += 8;
                for (int d = 0; d < 5 && gi->desc[d]; d++) {
                    draw_text(rr, TEXT_X, iy, gi->desc[d], 1, 105, 85, 70);
                    iy += 11;
                }
            } else {
                draw_text(rr, TEXT_X, iy, "No info available", 1, 80, 60, 50);
            }

            if (gi && gi->controls) {
                iy += 4;
                draw_text(rr, TEXT_X, iy, gi->controls, 1, 90, 140, 90);
            }

            int by = COVER_Y + COVER_H + 10;
            draw_text(rr, COVER_X, by, "150x40 LED  |  Intel 8048", 1, 60, 42, 35);
            draw_text(rr, COVER_X, by + 11, "COP411L Sound  |  15 fps", 1, 60, 42, 35);
        }

        SDL_SetRenderDrawColor(rr, 20, 8, 6, 255);
        { SDL_Rect r = { 0, MENU_LH - 20, MENU_LW, 20 }; SDL_RenderFillRect(rr, &r); }
        draw_text(rr, 14, MENU_LH - 15,
            "Select:Up/Down/Click  Play:Enter/DblClick  Esc:quit", 1, 80, 60, 48);

        /* ---- Blit render target to screen with letterboxing ---- */
        SDL_SetRenderTarget(rr, NULL);
        SDL_RenderSetScale(rr, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(rr, 6, 2, 2, 255);
        SDL_RenderClear(rr);
        SDL_RenderCopy(rr, rt, NULL, &blit_dst);
        SDL_RenderPresent(rr);
        SDL_Delay(30);
    }

menu_exit_play:
    {
        int result = m->selected;
#ifdef EMBED_COVERS
        for (int i = 0; i < m->game_count; i++)
            if (cover_tex[i]) SDL_DestroyTexture(cover_tex[i]);
#endif
        if (rt) SDL_DestroyTexture(rt);
        SDL_RenderSetViewport(rr, NULL);
        SDL_RenderSetScale(rr, 1.0f, 1.0f);
        SDL_RenderSetLogicalSize(rr, WIN_W, WIN_H);
        return result;
    }

menu_exit_quit:
#ifdef EMBED_COVERS
    for (int i = 0; i < m->game_count; i++)
        if (cover_tex[i]) SDL_DestroyTexture(cover_tex[i]);
#endif
    if (rt) SDL_DestroyTexture(rt);
    SDL_RenderSetViewport(rr, NULL);
    SDL_RenderSetScale(rr, 1.0f, 1.0f);
    SDL_RenderSetLogicalSize(rr, WIN_W, WIN_H);
    return -1;
}

/* ---- Audio callback — generates samples from COP411L engine ---- */
static void audio_cb(void *ud, uint8_t *stream, int len) {
    AV *av = (AV *)ud;
    int16_t *out = (int16_t *)stream;
    int n = len / (int)sizeof(int16_t);
    int vol = av->snd_volume;
    int amplitude = 300 * vol;  /* max 3000 at vol=10 */
    float prev = av->lp_prev;
    int prof = av->audio_profile;
    float alpha = (prof >= 0 && prof < AUDIO_PROFILES) ? audio_lp_alpha[prof] : 1.0f;
    for (int i = 0; i < n; i++) {
        float s = cop411_sample(&av->snd);
        /* Audio filter: profile-dependent low-pass */
        prev += alpha * (s - prev);
        float fout = prev;
        /* Soft clip for speaker profile (simulates small speaker distortion) */
        if (prof == AUDIO_SPEAKER && (fout > 0.8f || fout < -0.8f))
            fout = fout > 0 ? 0.8f + 0.2f * tanhf((fout-0.8f)*5.0f)
                            : -0.8f + 0.2f * tanhf((fout+0.8f)*5.0f);
        int16_t sample = (int16_t)(fout * (float)amplitude);
        out[i] = sample;
        /* Enqueue to WAV ring buffer (lock-free: single writer) */
        if (av->wav.active) {
            uint32_t wi = av->wav.ring_wr;
            av->wav.ring[wi & (WAV_RING_SZ - 1)] = sample;
            av->wav.ring_wr = wi + 1;
        }
    }
    av->lp_prev = prev;
}

/* ---- Render: faithful red LED POV display ----
 * Real hardware: 40 red LEDs + spinning mirror create discrete luminous
 * dots viewed through persistence of vision. Display is dark, designed
 * for dim rooms. Each LED appears as a round red dot with soft glow.
 * Color: warm LED red (not pure #FF0000), slightly orange at high
 * intensity, deep crimson when fading. */
static uint32_t framebuf[WIN_W * WIN_H];

/* Gamma lookup table: maps [0..255] intensity to gamma-corrected value.
 * Rebuilt when gamma setting changes. Eliminates powf() from render loop. */
static float gamma_lut[256];
static float gamma_lut_val = -1.0f;  /* current gamma, -1 = not initialized */

static void rebuild_gamma_lut(float gamma) {
    if (gamma == gamma_lut_val) return;
    gamma_lut_val = gamma;
    for (int i = 0; i < 256; i++) {
        float I = (float)i / 255.0f;
        gamma_lut[i] = (gamma != 1.0f) ? powf(I, gamma) : I;
    }
}

static void render(SDL_Renderer *rr, AV *av) {
    const AVDisp *d = &av->disp;

    /* Rebuild gamma LUT if setting changed */
    rebuild_gamma_lut(av->cfg_gamma);

    /* Count lit pixels for stats */
    int lit = 0;

    /* Render LED display into framebuffer */
    memset(framebuf, 0, sizeof(framebuf)); /* black background */

    for (int y = 0; y < SH; y++) {
        for (int x = 0; x < SW; x++) {
            float I = d->phosphor[x + y * SW];
            if (I < 0.01f) continue;
            lit++;

            /* Apply gamma via LUT (quantize to 8-bit, lookup) */
            int idx = (int)(I * 255.0f);
            if (idx > 255) idx = 255;
            float Ig = gamma_lut[idx];

            /* LED red color: warm at high intensity, deep crimson at low. */
            uint8_t r = (uint8_t)(Ig * 255.0f);
            uint8_t g = (uint8_t)(Ig * Ig * 25.0f);
            uint8_t b = (uint8_t)(Ig * Ig * Ig * 6.0f);
            uint32_t col = (r << 16) | (g << 8) | b;

            /* Fill sharp LED_SIZE×LED_SIZE dot within SCALE×SCALE cell.
             * The 1px black gap on right/bottom edge simulates the physical
             * spacing between discrete LEDs and mirror column positions. */
            int bx = x * SCALE, by = y * SCALE;
            for (int dy = 0; dy < LED_SIZE; dy++) {
                uint32_t *row = &framebuf[(by + dy) * WIN_W + bx];
                for (int dx = 0; dx < LED_SIZE; dx++)
                    row[dx] = col;
            }
        }
    }

    /* Upload framebuffer to texture and render */
    static SDL_Texture *game_tex = NULL;
    static SDL_Renderer *game_tex_rr = NULL;  /* track renderer for invalidation */
    if (!game_tex || game_tex_rr != rr) {
        if (game_tex) SDL_DestroyTexture(game_tex);
        game_tex = SDL_CreateTexture(rr, SDL_PIXELFORMAT_RGB888,
                                SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H);
        game_tex_rr = rr;
        if (!game_tex) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            return;
        }
    }
    SDL_UpdateTexture(game_tex, NULL, framebuf, WIN_W * sizeof(uint32_t));

    /* Clear full window (wipes any menu remnants from letterbox areas) */
    SDL_SetRenderDrawColor(rr, 0, 0, 0, 255);
    SDL_RenderClear(rr);

    if (av->integer_scale) {
        /* Integer scaling: temporarily bypass logical size to compute
         * integer multiples in actual output pixels, then letterbox. */
        int ow, oh;
        SDL_RenderSetLogicalSize(rr, 0, 0);  /* native coords */
        SDL_GetRendererOutputSize(rr, &ow, &oh);
        if (ow < 1) ow = 1;
        if (oh < 1) oh = 1;
        int sx = ow / WIN_W;
        int sy = oh / WIN_H;
        int s = (sx < sy) ? sx : sy;
        if (s < 1) s = 1;
        int dw = WIN_W * s, dh = WIN_H * s;
        SDL_Rect dst = { (ow - dw) / 2, (oh - dh) / 2, dw, dh };
        SDL_RenderCopy(rr, game_tex, NULL, &dst);
        SDL_RenderSetLogicalSize(rr, WIN_W, WIN_H);  /* restore */
    } else {
        SDL_RenderCopy(rr, game_tex, NULL, NULL);
    }

    /* Scanline effect: darken every other LED row */
    if (av->scanlines) {
        SDL_SetRenderDrawColor(rr, 0, 0, 0, 60);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_BLEND);
        for (int sy = 0; sy < SH; sy += 2) {
            SDL_Rect sl = {0, sy * SCALE, WIN_W, SCALE};
            SDL_RenderFillRect(rr, &sl);
        }
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_NONE);
    }

    /* Store stats */
    av->stat_pixels = lit;

    /* Stats overlay (FPS, cycles, pixels) */
    if (av->show_stats) {
        char sb[128];
        snprintf(sb, sizeof(sb), "FPS:%.1f Cy:%llu Px:%d",
            av->stat_fps, (unsigned long long)av->cpu.cycles, av->stat_pixels);
        SDL_SetRenderDrawColor(rr, 0, 0, 0, 180);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_BLEND);
        SDL_Rect sb_bg = {0, 0, (int)strlen(sb) * 7 + 8, 12};
        SDL_RenderFillRect(rr, &sb_bg);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_NONE);
        draw_text(rr, 4, 2, sb, 1, 100, 200, 100);
    }

    /* OSD overlay */
    if (av->osd_timer > 0) {
        av->osd_timer--;
        SDL_SetRenderDrawColor(rr, 0, 0, 0, 180);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_BLEND);
        SDL_Rect bg = { 8, WIN_H - 22, (int)strlen(av->osd_text) * 7 + 10, 16 };
        SDL_RenderFillRect(rr, &bg);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_NONE);
        draw_text(rr, 13, WIN_H - 19, av->osd_text, 1, 220, 220, 200);
    }

    /* Pause overlay */
    if (av->paused) {
        SDL_SetRenderDrawColor(rr, 0, 0, 0, 140);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_BLEND);
        SDL_Rect bg = { WIN_W/2 - 50, WIN_H/2 - 12, 100, 24 };
        SDL_RenderFillRect(rr, &bg);
        SDL_SetRenderDrawBlendMode(rr, SDL_BLENDMODE_NONE);
        draw_text(rr, WIN_W/2 - 38, WIN_H/2 - 6, "PAUSE", 2, 255, 200, 180);
    }

    SDL_RenderPresent(rr);
}

/* ---- Per-game save filename ---- */
static void make_save_name(char *out, int sz, const char *game_name) {
    char buf[64] = {0};
    int j = 0;
    for (int i = 0; game_name[i] && j < 60; i++) {
        char c = game_name[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) buf[j++] = c;
        else if (c == ' ' && j > 0 && buf[j-1] != '_') buf[j++] = '_';
    }
    buf[j] = '\0';
    snprintf(out, sz, "advision_%s.sav", j > 0 ? buf : "game");
}

/* ---- Main ---- */
int main(int argc, char **argv) {
    AV av;
    av_init(&av);

    /* Check for --test (works in SDL build too) */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--test") == 0) return run_self_test();

    /* Parse command line arguments */
    bool opt_fullscreen = false;
    bool opt_no_sound = false;
    int opt_scale = 0;
    int opt_volume = -1;  /* -1 = not set */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0) opt_fullscreen = true;
        else if (strcmp(argv[i], "--no-sound") == 0) opt_no_sound = true;
        else if (strcmp(argv[i], "--scale") == 0 && i+1 < argc) {
            char *end; long lv = strtol(argv[++i], &end, 10);
            if (*end == '\0' && lv >= 1 && lv <= 10) opt_scale = (int)lv;
            else fprintf(stderr, "Invalid --scale value, ignoring\n");
        }
        else if (strcmp(argv[i], "--volume") == 0 && i+1 < argc) {
            char *end; long lv = strtol(argv[++i], &end, 10);
            if (*end == '\0' && lv >= 0 && lv <= 10) opt_volume = (int)lv;
            else fprintf(stderr, "Invalid --volume value, ignoring\n");
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Adventure Vision Emulator v15\n\n"
                   "Usage: %s [options] [bios.rom game.rom]\n\n"
                   "Options:\n"
                   "  --fullscreen    Start in fullscreen\n"
                   "  --scale N       Window scale factor (1-10)\n"
                   "  --volume N      Initial volume (0-10, default 7)\n"
                   "  --no-sound      Disable audio\n"
                   "  --test          Run built-in self-test suite\n"
                   "  -h, --help      Show this help\n"
                   "\nHeadless options (no SDL):\n"
                   "  --frames N      Run N frames (default 60)\n"
                   "  --input UDLR    Inject inputs (U/D/L/R/1/2/3/4)\n"
                   "  --dump          Dump VRAM as ASCII art each frame\n", argv[0]);
            return 0;
        }
    }

    /* Load config (overridden by command line) */
    bool cfg_fs = false;
    config_load(&av, &cfg_fs);
    if (opt_fullscreen) cfg_fs = true;
    if (opt_scale) av.cfg_scale = opt_scale;
    if (opt_volume >= 0) av.snd_volume = opt_volume;  /* CLI overrides config */
    av.cfg_no_sound = opt_no_sound;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int init_w = 900, init_h = 540;
    if (av.cfg_scale > 0) {
        init_w = SW * av.cfg_scale;
        init_h = SH * av.cfg_scale;
    }
    SDL_Window *win = SDL_CreateWindow("Adventure Vision",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, init_w, init_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); return 1;
    }
    /* Set minimum size so menu remains usable */
    SDL_SetWindowMinimumSize(win, 640, 380);

    SDL_Renderer *rr = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rr) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit(); return 1;
    }
    SDL_RenderSetLogicalSize(rr, WIN_W, WIN_H);

    /* Audio */
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS;
    want.channels = 1; want.samples = AUDIO_SAMPLES;
    want.callback = audio_cb; want.userdata = &av;
    SDL_AudioDeviceID adev = 0;
    if (!av.cfg_no_sound) {
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (adev) SDL_PauseAudioDevice(adev, 0);
    }
    av.adev = (uint32_t)adev; /* Store for thread-safe audio locking in av_port_write */

    /* Gamepad */
    SDL_GameController *gp = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { gp = SDL_GameControllerOpen(i); break; }

    bool fullscreen = cfg_fs;
    if (fullscreen)
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);

    /* Determine direct mode: exactly 2 non-option args = bios + game */
    int pos_args = 0;
    char *pos_argv[2] = {NULL, NULL};
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && pos_args < 2) pos_argv[pos_args++] = argv[i];
        else if (strncmp(argv[i], "--scale", 7) == 0 || strncmp(argv[i], "--volume", 8) == 0) i++;
    }
    bool direct_mode = (pos_args >= 2);

    /* ===== OUTER LOOP: menu → game → menu ===== */
    while (1) {
        char game_title[256] = "Adventure Vision";

        /* Reset emulation state for new game, preserving persistent fields:
         * adev, rewind_buf, wav, config (volume, scale, gamma, etc.) */
        {
            /* Stop WAV if active before resetting */
            if (av.wav.active) {
                if (adev) SDL_LockAudioDevice(adev);
                av.wav.active = false;
                if (adev) SDL_UnlockAudioDevice(adev);
                wav_stop(&av.wav);
            }
            /* Save persistent fields */
            uint32_t      p_adev       = av.adev;
            RewindSnap   *p_rwbuf      = av.rewind_buf;
            int           p_volume     = av.snd_volume;
            int           p_scale      = av.cfg_scale;
            bool          p_no_sound   = av.cfg_no_sound;
            int           p_aprofile   = av.audio_profile;
            float         p_gamma      = av.cfg_gamma;
            float         p_phosphor   = av.cfg_phosphor;
            bool          p_scanlines  = av.scanlines;
            bool          p_intscale   = av.integer_scale;
            bool          p_stats      = av.show_stats;
            bool          p_midframe   = av.midframe_scan;
            int           p_t1start    = av.t1_pulse_start;
            int           p_t1end      = av.t1_pulse_end;
            /* Reset core state */
            memset(&av.cpu, 0, sizeof(I8048));
            memset(&av.disp, 0, sizeof(AVDisp));
            memset(&av.input, 0, sizeof(av.input));
            av.running = true;
            av.paused = false;
            av.back_to_menu = false;
            av.frame_count = 0;
            av.osd_text[0] = 0;
            av.osd_timer = 0;
            av.stat_frame_ticks = 0;
            av.stat_fps = 0;
            av.stat_pixels = 0;
            memset(&av.dbg, 0, sizeof(av.dbg));
            av.dbg_run_to = 0xFFFF;
            av.dbg_watch_addr = 0xFFFF;
            av.dbg_watch_en = false;
            av.lp_prev = 0;
            /* Init CPU */
            av.cpu.P1 = 0xFB;
            av.cpu.P2 = 0xFF;
            av.cpu.t0 = true;
            memset(av.cpu.xram + 0x100, 0xFF, 0x300);
            /* Init sound (under lock since audio thread may be running) */
            if (adev) SDL_LockAudioDevice(adev);
            cop411_init(&av.snd);
            if (adev) SDL_UnlockAudioDevice(adev);
            snprintf(av.save_name, sizeof(av.save_name), "advision.sav");
            /* Restore persistent fields */
            av.adev          = p_adev;
            av.rewind_buf    = p_rwbuf;
            av.rewind_head   = 0;
            av.rewind_count  = 0;
            av.snd_volume    = p_volume;
            av.cfg_scale     = p_scale;
            av.cfg_no_sound  = p_no_sound;
            av.audio_profile = p_aprofile;
            av.cfg_gamma     = p_gamma;
            av.cfg_phosphor  = p_phosphor;
            av.scanlines     = p_scanlines;
            av.integer_scale = p_intscale;
            av.show_stats    = p_stats;
            av.midframe_scan = p_midframe;
            av.t1_pulse_start= p_t1start;
            av.t1_pulse_end  = p_t1end;
        }

        if (direct_mode) {
            if (!load_file(av.cpu.irom, IROM_SZ, pos_argv[0])) break;
            if (!load_file(av.cpu.erom, EROM_SZ, pos_argv[1])) break;
            const char *slash = strrchr(pos_argv[1], '/');
            const char *gn = prettify_name(slash ? slash+1 : pos_argv[1]);
            snprintf(game_title, sizeof(game_title), "Adventure Vision - %.200s", gn);
            make_save_name(av.save_name, sizeof(av.save_name), gn);
        } else {
            GameMenu menu;
            memset(&menu, 0, sizeof(menu));
            menu_scan(&menu, ".");

            if (menu.game_count == 0 && !menu.has_bios) {
                fprintf(stderr,
                    "Entex Adventure Vision Emulator v15\n\n"
                    "Usage: %s [bios.rom game.rom]\n\n"
                    "  Or place .bin/.rom files in current directory.\n\n"
                    "Controls: Arrows=D-Pad  Z/X/A/S=Buttons  Esc=Menu\n"
                    "  P=Pause  R=Reset  F5=Save  F7=Load  F11=Fullscreen\n"
                    "  +/-=Volume\n",
                    argv[0]);
                break;
            }

            int sel;
            if (menu.game_count == 1 && menu.has_bios) {
                sel = 0;
            } else {
                sel = menu_run(&menu, rr, win);
                if (sel < 0) break;
            }

            /* Load BIOS */
#ifdef EMBED_ROMS
            if (menu.bios_embedded) {
                int bsz = (int)sizeof(embedded_bios);
                if (bsz > IROM_SZ) bsz = IROM_SZ;
                memcpy(av.cpu.irom, embedded_bios, bsz);
            } else
#endif
            {
                if (!load_file(av.cpu.irom, IROM_SZ, menu.bios_path)) break;
            }

            /* Load game */
            if (menu.game_embed_idx[sel] >= 0) {
#ifdef EMBED_ROMS
                int idx = menu.game_embed_idx[sel];
                int gsz = embedded_games[idx].size;
                if (gsz > EROM_SZ) gsz = EROM_SZ;
                memcpy(av.cpu.erom, embedded_games[idx].data, gsz);
#endif
            } else {
                if (!load_file(av.cpu.erom, EROM_SZ, menu.game_paths[sel])) break;
            }

            snprintf(game_title, sizeof(game_title), "Adventure Vision - %.200s", menu.game_names[sel]);
            make_save_name(av.save_name, sizeof(av.save_name), menu.game_names[sel]);
        }

        SDL_SetWindowTitle(win, game_title);
        av.back_to_menu = false;

        /* ===== GAME LOOP ===== */
        while (av.running && !av.back_to_menu) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_QUIT: av.running = false; break;
                case SDL_KEYDOWN: case SDL_KEYUP: {
                    bool p = (e.type == SDL_KEYDOWN);
                    switch (e.key.keysym.sym) {
                    case SDLK_UP:    av.input.u=p; break;
                    case SDLK_DOWN:  av.input.d=p; break;
                    case SDLK_LEFT:  av.input.l=p; break;
                    case SDLK_RIGHT: av.input.r=p; break;
                    case SDLK_z: av.input.b1=p; break;
                    case SDLK_x: av.input.b2=p; break;
                    case SDLK_a: av.input.b3=p; break;
                    case SDLK_s: av.input.b4=p; break;
                    case SDLK_ESCAPE:
                        if (p) {
                            if (direct_mode) av.running = false;
                            else av.back_to_menu = true;
                        }
                        break;
                    case SDLK_p:
                        if (p) {
                            av.paused = !av.paused;
                            osd_show(&av, av.paused ? "Paused" : "Resumed");
                        }
                        break;
                    case SDLK_r:
                        if (p) { av_reset(&av); osd_show(&av, "Reset"); }
                        break;
                    case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS:
                        if (p && av.snd_volume < 10) {
                            av.snd_volume++;
                            char buf[32]; snprintf(buf, 32, "Volume: %d", av.snd_volume);
                            osd_show(&av, buf);
                        }
                        break;
                    case SDLK_MINUS: case SDLK_KP_MINUS:
                        if (p && av.snd_volume > 0) {
                            av.snd_volume--;
                            char buf[32]; snprintf(buf, 32, "Volume: %d", av.snd_volume);
                            osd_show(&av, buf);
                        }
                        break;
                    case SDLK_F1:
                        if(p){ av.dbg.active=!av.dbg.active;
                               printf("[DBG] %s\n",av.dbg.active?"ON":"OFF");
                               if(av.dbg.active)dbg_print(&av.cpu); }
                        break;
                    case SDLK_BACKQUOTE:
                        if(p) {
                            av.show_stats = !av.show_stats;
                            osd_show(&av, av.show_stats ? "Stats ON" : "Stats OFF");
                        }
                        break;
                    case SDLK_F2:
                        if(p) {
                            if (av.wav.active) {
                                AUDIO_LOCK(&av);
                                av.wav.active = false;  /* signal audio thread to stop writing */
                                AUDIO_UNLOCK(&av);
                                wav_stop(&av.wav);
                                osd_show(&av, "WAV saved");
                            } else {
                                time_t now = time(NULL);
                                struct tm *t = localtime(&now);
                                char wfn[128];
                                snprintf(wfn, sizeof(wfn), "advision_%04d%02d%02d_%02d%02d%02d.wav",
                                    t->tm_year+1900, t->tm_mon+1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
                                if (adev) SDL_LockAudioDevice(adev);
                                wav_start(&av.wav, wfn);
                                if (adev) SDL_UnlockAudioDevice(adev);
                                osd_show(&av, av.wav.active ? "Recording WAV..." : "WAV failed");
                            }
                        }
                        break;
                    case SDLK_F3:
                        if(p) {
                            av.midframe_scan = !av.midframe_scan;
                            osd_show(&av, av.midframe_scan ? "Mid-frame scan ON" : "Mid-frame scan OFF");
                        }
                        break;
                    case SDLK_F4:
                        if(p) {
                            av.audio_profile = (av.audio_profile + 1) % AUDIO_PROFILES;
                            char apb[48]; snprintf(apb, 48, "Audio: %s", audio_profile_names[av.audio_profile]);
                            osd_show(&av, apb);
                        }
                        break;
                    case SDLK_F5:
                        if(p) {
                            AUDIO_LOCK(&av);
                            bool saved = save_state(&av, av.save_name);
                            AUDIO_UNLOCK(&av);
                            osd_show(&av, saved ? "State saved" : "Save failed!");
                        }
                        break;
                    case SDLK_F6:
                        if(p) {
                            av.integer_scale = !av.integer_scale;
                            osd_show(&av, av.integer_scale ? "Integer scale ON" : "Integer scale OFF");
                        }
                        break;
                    case SDLK_F7:
                        if(p) {
                            AUDIO_LOCK(&av);
                            bool loaded = load_state(&av, av.save_name);
                            AUDIO_UNLOCK(&av);
                            osd_show(&av, loaded ? "State loaded" : "No save found");
                        }
                        break;
                    case SDLK_F8:
                        if(p) {
                            /* Rewind: pop multiple frames for visible effect */
                            AUDIO_LOCK(&av);
                            int rw = 0;
                            for (int ri = 0; ri < 4; ri++)
                                if (rewind_pop(&av)) rw++;
                            AUDIO_UNLOCK(&av);
                            if (rw > 0) {
                                char rb[32]; snprintf(rb, 32, "Rewind -%d", rw);
                                osd_show(&av, rb);
                            } else osd_show(&av, "No rewind data");
                        }
                        break;
                    case SDLK_F9:
                        if(p) {
                            if (av.dbg.active && av.dbg.stepping) {
                                i8048_exec(&av.cpu,&av); dbg_print(&av.cpu);
                            } else {
                                av.scanlines = !av.scanlines;
                                osd_show(&av, av.scanlines ? "Scanlines ON" : "Scanlines OFF");
                            }
                        }
                        break;
                    case SDLK_F10:
                        if(p && av.dbg.active) av.dbg.stepping=false;
                        break;
                    case SDLK_F11:
                        if(p) {
                            fullscreen = !fullscreen;
                            SDL_SetWindowFullscreen(win,
                                fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                        }
                        break;
                    case SDLK_F12:
                        if(p) { screenshot_bmp(rr); osd_show(&av, "Screenshot saved"); }
                        break;
                    default: break;
                    } break;
                }
                case SDL_MOUSEBUTTONDOWN:
                    /* Double-click toggles borderless fullscreen */
                    if (e.button.button == SDL_BUTTON_LEFT && e.button.clicks == 2) {
                        fullscreen = !fullscreen;
                        SDL_SetWindowFullscreen(win,
                            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    }
                    break;
                case SDL_DROPFILE: {
                    /* Drag & drop: load ROM file into game slot */
                    char *drop = e.drop.file;
                    if (drop) {
                        long dsz = 0;
                        FILE *df = fopen(drop, "rb");
                        if (df) { fseek(df, 0, SEEK_END); dsz = ftell(df); fclose(df); }
                        if (dsz == 1024) {
                            load_file(av.cpu.irom, IROM_SZ, drop);
                            osd_show(&av, "BIOS loaded");
                        } else if (dsz >= 512 && dsz <= 8192) {
                            load_file(av.cpu.erom, EROM_SZ, drop);
                            av_reset(&av);
                            osd_show(&av, "ROM loaded & reset");
                        }
                        SDL_free(drop);
                    }
                    break;
                }
                case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
                    bool p = (e.type == SDL_CONTROLLERBUTTONDOWN);
                    switch(e.cbutton.button){
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    av.input.u=p; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  av.input.d=p; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  av.input.l=p; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: av.input.r=p; break;
                    case SDL_CONTROLLER_BUTTON_A: av.input.b1=p; break;
                    case SDL_CONTROLLER_BUTTON_B: av.input.b2=p; break;
                    case SDL_CONTROLLER_BUTTON_X: av.input.b3=p; break;
                    case SDL_CONTROLLER_BUTTON_Y: av.input.b4=p; break;
                    case SDL_CONTROLLER_BUTTON_START:
                        if(p){ av.paused=!av.paused; osd_show(&av, av.paused?"Paused":"Resumed"); }
                        break;
                    case SDL_CONTROLLER_BUTTON_BACK:
                        if(p) av.back_to_menu = !direct_mode;
                        break;
                    default: break;
                    } break;
                }
                case SDL_CONTROLLERDEVICEADDED:
                    if(!gp) gp = SDL_GameControllerOpen(e.cdevice.which);
                    break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if(gp && e.cdevice.which == SDL_JoystickInstanceID(
                        SDL_GameControllerGetJoystick(gp))) {
                        SDL_GameControllerClose(gp); gp=NULL;
                    }
                    break;
                default: break;
                }
            }

            if(gp){
                int16_t lx = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_LEFTX);
                int16_t ly = SDL_GameControllerGetAxis(gp, SDL_CONTROLLER_AXIS_LEFTY);
                /* Analog stick: only override if stick is outside deadzone,
                 * otherwise leave keyboard-driven state untouched */
                if (lx < -8000 || lx > 8000) {
                    av.input.l = (lx < -8000);
                    av.input.r = (lx >  8000);
                }
                if (ly < -8000 || ly > 8000) {
                    av.input.u = (ly < -8000);
                    av.input.d = (ly >  8000);
                }
            }

            if(!av.paused && !av.dbg.stepping)
                av_run_frame(&av);

            render(rr, &av);

            /* Flush WAV ring buffer to disk (main thread only) */
            if (av.wav.fp) wav_flush(&av.wav);

            /* Frame timing + FPS measurement */
            {
                static Uint32 last_tick = 0;
                Uint32 now = SDL_GetTicks();
                if (!last_tick || (now - last_tick) > 500)
                    last_tick = now;
                /* Measure FPS (smoothed) */
                Uint32 dt = now - av.stat_frame_ticks;
                if (dt > 0 && dt < 500) {
                    float ifps = 1000.0f / (float)dt;
                    av.stat_fps = av.stat_fps * 0.9f + ifps * 0.1f;
                }
                av.stat_frame_ticks = now;
                Uint32 target = last_tick + (1000 / FPS);
                if (now < target)
                    SDL_Delay(target - now);
                last_tick = SDL_GetTicks();
            }
        }

        if (!av.running || direct_mode) break;
        SDL_SetWindowTitle(win, "Adventure Vision");
    }

    /* Save config on clean exit */
    config_save(&av, fullscreen);

    /* Stop WAV recording if active */
    if (av.wav.active) {
        if (adev) SDL_LockAudioDevice(adev);
        av.wav.active = false;
        if (adev) SDL_UnlockAudioDevice(adev);
        wav_stop(&av.wav);
    }

    /* Free rewind buffer */
    if (av.rewind_buf) { free(av.rewind_buf); av.rewind_buf = NULL; }

    if(adev) SDL_CloseAudioDevice(adev);
    if(gp) SDL_GameControllerClose(gp);
    SDL_DestroyRenderer(rr);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

#else
/* Headless mode */
int main(int argc, char **argv) {
    /* Check for --test flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return run_self_test();
    }

    /* Parse headless options */
    int num_frames = 60;
    const char *input_str = NULL;
    bool do_dump = false;
    char *bios_path = NULL, *game_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i+1 < argc) {
            char *end; long lv = strtol(argv[++i], &end, 10);
            if (*end == '\0' && lv > 0 && lv < 1000000) num_frames = (int)lv;
        }
        else if (strcmp(argv[i], "--input") == 0 && i+1 < argc)
            input_str = argv[++i];
        else if (strcmp(argv[i], "--dump") == 0)
            do_dump = true;
        else if (argv[i][0] != '-') {
            if (!bios_path) bios_path = argv[i];
            else if (!game_path) game_path = argv[i];
        }
    }

    if (!bios_path || !game_path) {
        printf("Usage: %s [--test] [--frames N] [--input UDLR1234] [--dump] <bios.rom> <game.rom>\n", argv[0]);
        return 1;
    }

    AV av; av_init(&av);
    if (!load_file(av.cpu.irom, IROM_SZ, bios_path)) return 1;
    if (!load_file(av.cpu.erom, EROM_SZ, game_path)) return 1;

    /* Apply input string */
    if (input_str) {
        for (const char *p = input_str; *p; p++) {
            switch (*p) {
            case 'U': case 'u': av.input.u = true; break;
            case 'D': case 'd': av.input.d = true; break;
            case 'L': case 'l': av.input.l = true; break;
            case 'R': case 'r': av.input.r = true; break;
            case '1': av.input.b1 = true; break;
            case '2': av.input.b2 = true; break;
            case '3': av.input.b3 = true; break;
            case '4': av.input.b4 = true; break;
            }
        }
    }

    for (int f = 0; f < num_frames; f++) {
        av_run_frame(&av);
        if (do_dump) {
            printf("--- Frame %d ---\n", f);
            dump_vram_ascii(&av.disp);
        }
    }

    dbg_print(&av.cpu);
    int lit = 0;
    for (int i = 0; i < SW*SH; i++) if (av.disp.phosphor[i] > 0.1f) lit++;
    printf("%llu cycles, %d pixels lit, %d frames.\n",
        (unsigned long long)av.cpu.cycles, lit, num_frames);
    if (av.rewind_buf) free(av.rewind_buf);
    return 0;
}
#endif
