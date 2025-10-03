/*
 * A very simple filter (with minimal error checking) to
 * convert previews in their custom YYCbCr format into
 * something more common like PPM.
 *
 * Written by Thierry Bousch <bousch@topo.math.u-psud.fr>
 * and released in the public domain.
 *
 * $Id: yycc2ppm.c,v 1.2 1998/07/28 21:30:56 bousch Exp $
 */

#include <stdio.h>

static int putcc (int x)
{
	if (x < 0)
	  x = 0;
	if (x > 255)
	  x = 255;
	return putchar(x);
}

int main (void)
{
	unsigned char c[4];
	int width, height, Y1, Y2, Cb, Cr, Roff, Goff, Boff;

	if (fread(c,1,4,stdin) != 4)
		return 1;
	width  = c[0] + 256 * c[1];
	height = c[2] + 256 * c[3];
	printf("P6\n%d %d\n255\n", width, height);
	while (fread(c,1,4,stdin) == 4) {
		Y1 = c[0];
		Y2 = c[1];
		Cb = c[2] - 128;
		Cr = c[3] - 128;
		Roff = (359*Cr + 128) >> 8;
		Goff = (-88*Cb -183*Cr + 128) >> 8;
		Boff = (454*Cb + 128) >> 8;
		putcc(Y1+Roff); putcc(Y1+Goff); putcc(Y1+Boff);
		putcc(Y2+Roff); putcc(Y2+Goff); putcc(Y2+Boff);
	}
	return 0;
}
