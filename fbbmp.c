/* Linux VGA16 driver bitmap viewer
 * 
 * Just make sure that you have VGA16 enabled
 * In the kernel configuration, and just run
 * it like this
 *
 * ./fbbmp image.bmp
 *
 * NOTE: this is really ugly, and just a starting point. Also, it needs uncompressed 4bpp bitmaps only. Do not yell at me in the github issues about this, I might fix it one day with dithering.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/io.h>

#include <linux/fb.h>
#include <linux/kd.h>

#define FB_DEVICE "/dev/fb0"
#define TTY_DEVICE "/dev/tty0"

/*--------------------*/
/* BMP structures     */
/*--------------------*/

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

#pragma pack(1)

typedef struct {
	u16 bfType;
	u32 bfSize;
	u16 bfReserved1;
	u16 brReserved2;
	u32 bfOffBits;
} BMPFILEHEADER;

typedef struct {
	u32 biSize;
	long biWidth;
	long biHeight;
	u16 biPlanes;
	u16 biBitCount;
	u32 biCompression;
	u32 biSizeImage;
	long biXPelsPerMeter;
	long biYPelsPerMeter;
	u32 biClrUsed;
	u32 biClrImportant;
} BMPINFOHEADER;

typedef struct {
	u8 b;
	u8 g;
	u8 r;
	u8 reserved;
} RGBQUAD;

#pragma pack()

/*--------------------*/

static int fbfd;
static int ttyfd;

static struct fb_fix_screeninfo finfo;
static struct fb_var_screeninfo vinfo;

static u8 *fbmem;

/* VGA register ports */
#define SEQ_INDEX	0x3C4
#define SEQ_DATA	0x3C5

/*--------------------*/

static void set_plane(int plane)
{
	if (ioperm(0x3C4, 2, 1)) {
		perror("ioperm");
		return 1;
	}	

	outb(0x02, SEQ_INDEX);
	outb(1 << plane, SEQ_DATA);
}

/*--------------------*/

static void write_planar_byte(
	int xbyte,
	int y,
	u8 plane0,
	u8 plane1,
	u8 plane2,
	u8 plane3,)
{
	long offset;

	offset = (y * finfo.line_length) + xbyte;

	set_plane(0);
	fbmem[offset] = plane0;

	set_plane(1);
	fbmem[offset] = plane1;

	set_plane(2);
	fbmem[offset] = plane2;

	set_plane(3);
	fbmem[offset] = plane3;
}

/*--------------------*/

static void putpixel(int x, int y, int color)
{
	int plane;
	int offset;
	int bit;

	if (x < 0 || y < 0)
		return;

	if (x >= (int)vinfo.xres || y >= (int)vinfo.yres)
		return;

	offset = (y * finfo.line_length) + (x >> 3);
	bit = 7 - (x & 7);

	for (plane = 0; plane < 4; plane++) {

	u8 mask;
	u8 *ptr;

	set_plane(plane);

	ptr = fbmem + offset;

	mask = 1 << bit;

	if (color & (1 << plane))
		*ptr |= mask;
	else
		*ptr &= ~mask;
	}
}

/*--------------------*/

static int load_bmp(const char *filename)
{
	FILE *fp;

	BMPFILEHEADER bfh;
	BMPINFOHEADER bih;

	RGBQUAD palette[16];

	int x;
	int y;

	int rowbytes;

	u8 *rowbuf;

	fp = fopen(filename, "rb");

	if (!fp) {
		perror("fopen");
		return -1;
	}

	fread(&bfh, sizeof(bfh), 1, fp);
	fread(&bih, sizeof(bih), 1, fp);

	if (bfh.bfType != 0x4D42) {
		printf("Not a BMP file!\n");
		fclose(fp);
		return -1;
	}
	
	/* vmunix note: You could probably turn this off, but I don't know, it might blow up your graphics card or it might SEGFAULT. No clue as to what it would do. Comment out if you really want to. */
	if (bih.biBitCount != 4) {
		printf("Only 4bpp BMP supported!\n");
		fclose(fp);
		return -1;
	}

	if (bih.biCompression != 0) {
		printf("Compressed BMP not supported!\n");
		fclose(fp);
		return -1;
	}

	fread(palette, sizeof(RGBQUAD), 16, fp);

	rowbytes = ((bih.biWidth + 1) / 2 + 3) & ~3;

	rowbuf = (u8 *)malloc(rowbytes);

	if (!rowbuf) {
		printf("malloc failed!\n");
		printf("Yikes...\n"); /* vmunix: rofl */
		fclose(fp);
		return -1;
	}

	fseek(fp, bfh.bfOffBits, SEEK_SET);

	for (y = 0; y < bih.biHeight; y++) {

		int dsty;

		dsty = bih.biHeight - 1 - y;

		fread(rowbuf, 1, rowbytes, fp);

		for (x = 0; x < bih.biWidth; x += 8) {

			int i;

			u8 p0;
			u8 p1;
			u8 p2;
			u8 p3;

			p0 = 0;
			p1 = 0;
			p2 = 0;
			p3 = 0;

			for (i = 0; i < 8; i++) {

				int px;
				int color;

				u8 byte;

				px = x + i;

				if (px >= bih.biWidth)
					break;

				byte = rowbuf[px / 2];

				if (px & 1)
					color = byte & 0x0F;
				else
					color = (byte >> 4) & 0x0F;

				if (color & 1)
					p0 |= (1 << (7 - i));

				if (color & 2)
					p1 |= (1 << (7 - i));

				if (color & 4)
					p2 |= (1 << (7 - i));

				if (color & 8)
					p3 |= (1 << (7 - i));
			}

			write_planar_byte(
				x >> 3,
				dsty,
				p0,
				p1,
				p2,
				p3,
			);
		}
	}

	free(rowbuf);

	fclose(fp);

	return 0;
}

/*--------------------*/

int main(int argc, char *argv[])
{
	long screensize;

	if (argc < 2) {
		printf("usage: %s [image.bmp]\n", argv[0]);
		return 1;
	}

	if (ioperm(0x3C4, 2, 1)) {
		perror("ioperm");
		return 1;
	}

	fbfd = open(FB_DEVICE, O_RDWR);

	if (fbfd < 0) {
		perror("The default framebuffer specified in the source file could not be opened. Either compile the VGA16 framebuffer into the kernel, or run 'modprobe vga16fb'.");
		return 1;
	}

	ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
	ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);

	printf("Framebuffer: %dx%d %dbpp\n",
	    vinfo.xres,
	    vinfo.yres,
	    vinfo.bits_per_pixel);

	screensize = finfo.smem_len;

	fbmem = (u8 *)mmap(
	    0,
	    screensize,
	    PROT_READ | PROT_WRITE,
	    MAP_SHARED,
	    fbfd,
	    0
	);

	if ((long)fbmem == -1) {
		perror("Framebuffer memory is -1 bytes. This is not good.\n");
		close(fbfd);
		return 1;
	}

	ttyfd = open(TTY_DEVICE, O_RDWR);

	if (ttyfd >= 0)
		ioctl(ttyfd, KDSETMODE, KD_GRAPHICS);

	memset(fbmem, 0, screensize);

	load_bmp(argv[1]);

	printf("Press ENTER to exit...\n");
	getchar();

	if (ttyfd >= 0) {
		ioctl(ttyfd, KDSETMODE, KD_TEXT);
		close(ttyfd);
	}

	munmap(fbmem, screensize);

	close(fbfd);

	return 0;
}
		
