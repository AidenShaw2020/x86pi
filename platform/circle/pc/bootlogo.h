/*
 * The boot logo: what the screen shows while the machine is coming up, before
 * the guest's BIOS takes the display over.
 *
 * The picture itself is in bootlogo.c, which is generated from
 * platform/circle/bootlogo.png by platform/circle/tools/mklogo.py - see there
 * for why it is 16 bit and packed rather than a plain array.
 */
#ifndef BOOTLOGO_H
#define BOOTLOGO_H

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned short bootlogo_width;
extern const unsigned short bootlogo_height;
extern const unsigned int bootlogo_packed_size;
extern const unsigned char bootlogo_packed[];

/*
 * Unpack the logo into a 32 bit framebuffer, centred, leaving the rest of it
 * as it is.  The pixels are written as 0x00RRGGBB, which is what the VGA
 * renderer writes; `pitch` is in bytes.  Needs about half a megabyte of heap
 * while it runs, and gives it back.  Nothing happens, and nothing breaks, if
 * that is not there.
 */
void bootlogo_draw(void *framebuffer, unsigned width, unsigned height,
                   unsigned pitch);

#ifdef __cplusplus
}
#endif

#endif
