// stdio_stubs.c — no-op overrides of libc stdio output functions.
//
// The doomgeneric engine has 220+ printf/fprintf/puts/putchar calls in
// the unmodified upstream source (banner messages, IWAD diagnostics,
// demo-version mismatch warnings, NET_Init chatter, etc). In a normal
// terminal/SDL build these print to stdout. In an nx-ovlloader sysmodule
// there is no stdout — newlib's stdout FILE* has no valid write hook,
// and the first call into vfprintf/__sbprintf dereferences NULL and
// brings down the whole OS via Atmosphère.
//
// Confirmed root cause for the title-screen-after-N-seconds OS crash:
// G_DoPlayDemo (g_game.c:2184) does printf("Demo is from a different
// game version!\n", ...) when freedoom1.wad's attract demo lump fails
// the chocolate-doom version-code check. Crash report PC was inside
// __sbprintf with LR pointing at M_CheckParmWithArgs (printf internals
// looking up format-string args).
//
// Patching all 220 sites with a patch file is impractical. Overriding
// the public stdio API at link time is the systemic fix — the linker
// resolves these symbols from our object files first, so libc's
// vfprintf/__sbprintf are never pulled in or called.
//
// What this means in practice: engine init banners and warnings are
// silently dropped. Anything that previously printed-and-continued is
// now no-op-and-continues (same observable behavior since nothing was
// reading the output anyway). Anything that printed-and-then-errored
// (e.g. I_Error fires AFTER a warning printf) still errors via the
// I_Error path, which we catch via setjmp recovery.
//
// If a specific engine print becomes useful for debugging later, route
// just that one to source/main.cpp's doom_trace() helper rather than
// re-enabling libc stdio globally — stdio is fundamentally broken in
// this runtime.

#include <stdio.h>
#include <stdarg.h>

int printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    (void)stream;
    (void)fmt;
    return 0;
}

int vprintf(const char *fmt, va_list ap) {
    (void)fmt;
    (void)ap;
    return 0;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    (void)stream;
    (void)fmt;
    (void)ap;
    return 0;
}

int puts(const char *s) {
    (void)s;
    return 0;
}

int putchar(int c) {
    return c;
}

int fputs(const char *s, FILE *stream) {
    (void)s;
    (void)stream;
    return 0;
}

int fputc(int c, FILE *stream) {
    (void)stream;
    return c;
}

int putc(int c, FILE *stream) {
    (void)stream;
    return c;
}

void perror(const char *s) {
    (void)s;
}
