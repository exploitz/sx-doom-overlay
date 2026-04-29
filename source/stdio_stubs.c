// stdio_stubs.c — no-op overrides of libc stdio output functions.
//
// doomgeneric has 220+ printf/fprintf/puts calls. In an nx-ovlloader
// sysmodule there is no stdout — newlib's stdout FILE* has no valid write
// hook, and the first call into vfprintf/__sbprintf dereferences NULL and
// crashes Atmosphère.
//
// Confirmed root cause: G_DoPlayDemo (g_game.c:2184) does printf("Demo is
// from a different game version!\n") when the attract demo version-code
// check fails. Crash PC was inside __sbprintf with LR inside M_CheckParmWithArgs.
//
// Patching all 220 sites is impractical. Overriding the public stdio API at
// link time is the systemic fix — the linker resolves these symbols from our
// object first, so libc's vfprintf/__sbprintf are never pulled in or called.
//
// Engine init banners and warnings are silently dropped. Anything that
// printed-and-continued now no-ops-and-continues (same behavior since nothing
// was reading stdout). Anything that printed-and-errored still errors via I_Error
// → setjmp recovery.

#include <stdio.h>
#include <stdarg.h>

int printf(const char *fmt, ...) { (void)fmt; return 0; }
int fprintf(FILE *stream, const char *fmt, ...) { (void)stream; (void)fmt; return 0; }
int vprintf(const char *fmt, va_list ap) { (void)fmt; (void)ap; return 0; }
int vfprintf(FILE *stream, const char *fmt, va_list ap) { (void)stream; (void)fmt; (void)ap; return 0; }
int puts(const char *s) { (void)s; return 0; }
int putchar(int c) { return c; }
int fputs(const char *s, FILE *stream) { (void)s; (void)stream; return 0; }
int fputc(int c, FILE *stream) { (void)stream; return c; }
int putc(int c, FILE *stream) { (void)stream; return c; }
void perror(const char *s) { (void)s; }
