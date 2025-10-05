/*
 * A program to control Fujifilm digital cameras, like
 * the DS-7 and MX-700, and their clones.
 *
 * $Id: fujiplay.c,v 1.33 1999/02/22 14:13:01 bousch Exp $
 *
 * Written by Thierry Bousch <bousch@topo.math.u-psud.fr>
 * and released in the public domain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#ifndef CLK_TCK
#include <sys/param.h>
#define CLK_TCK HZ
#endif

#if !defined(B57600) && defined(EXTA)
#define B57600 EXTA
#endif

#if !defined(B115200) && defined(EXTB)
#define B115200 EXTB
#endif

#define DEFAULT_DEVICE	"/dev/fujifilm"
#define TMP_PIC_FILE	".dsc_temp"

struct pict_info {
	char *name;
	int number;
	int size;
	short ondisk;
	short transferred;
};

struct baudrate_info {
	int number;
	int posix_speed;
	int speed;
};

struct baudrate_info brinfo[] = {
#ifdef B115200
	{ 8, B115200, 115200 },
#endif
#ifdef B57600
	{ 7,  B57600,  57600 },
#endif
	{ 6,  B38400,  38400 },
	{ 4,  B19200,  19200 },
	{ 0,   B9600,   9600 }
};

int devfd = -1;
int desired_speed = -1;
int list_command_set = 0;
int maxnum;
struct termios oldt, newt;
char has_cmd[256];
int pictures;
int interrupted = 0;
int pending_input = 0;
struct pict_info *pinfo = NULL;

unsigned char answer[5000];
int answer_len = 0;

static int get_raw_byte (void)
{
	static unsigned char buffer[128];
	static unsigned char *bufstart;
	int ret;

	while (!pending_input) {
		/* Refill the buffer */
		ret = read(devfd, buffer, 128);
		if (ret == 0)
			return -1;  /* timeout */
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;  /* error */
		}
		pending_input = ret;
		bufstart = buffer;
	}
	pending_input--;
	return *bufstart++;
}

int wait_for_input (int seconds)
{
	fd_set rfds;
	struct timeval tv;

	if (pending_input)
		return 1;
	if (!seconds)
		return 0;

	FD_ZERO(&rfds);
	FD_SET(devfd, &rfds);
	tv.tv_sec = seconds;
	tv.tv_usec = 0;

	return select(1+devfd, &rfds, NULL, NULL, &tv);
}

int get_byte (void)
{
	int c;

	c = get_raw_byte();
	if (c < 255)
		return c;
	c = get_raw_byte();
	if (c == 255)
		return c;	/* escaped '\377' */
	if (c != 0)
		fprintf(stderr, "get_byte: impossible escape sequence following 0xFF\n");
	/* Otherwise, it's a parity or framing error */
	get_raw_byte();
	return -1;
}

int put_bytes (int n, unsigned char* buff)
{
	int ret;

	while (n > 0) {
		ret = write(devfd, buff, n);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		n -= ret;
		buff += ret;
	}
	return 0;
}

int put_byte (int c)
{
	unsigned char buff[1];

	buff[0] = c;
	return put_bytes(1, buff);
}

int attention (void)
{
	int i;

	/* drain input */
	while (get_byte() >= 0)
		continue;
	for (i = 0; i < 3; i++) {
		put_byte(0x05);
		if (get_byte() == 0x06)
			return 0;
	}
	fprintf(stderr, "The camera does not respond.\n");
	exit(1);
}

void send_packet (int len, unsigned char *data, int last)
{
	unsigned char *p, *end, buff[3];
	int check;

	last = last ? 0x03 : 0x17;
	check = last;
	end = data + len;
	for (p = data; p < end; p++)
		check ^= *p;

	/* Start of frame */
	buff[0] = 0x10;
	buff[1] = 0x02;
	put_bytes(2, buff);

	/* Data */
	for (p = data; p < end; p++)
		if (*p == 0x10) {
			/* Send everything between "data" and "p" (included) */
			put_bytes(p-data+1, data);
			/* And make sure the last byte (0x10) will be sent again */
			data = p;
		}
	put_bytes(end-data, data);

	/* End of frame */
	buff[1] = last;
	buff[2] = check;
	put_bytes(3, buff);
}

int read_packet (void)
{
	unsigned char *p = answer;
	int c, check, incomplete;

	if (get_byte() != 0x10 || get_byte() != 0x02) {
bad_frame:
		/* drain input */
		while (get_byte() >= 0)
			continue;
		return -1;
	}
	check = 0;
	while(1) {
		if ((c = get_byte()) < 0)
			goto bad_frame;
		if (c == 0x10) {
			if ((c = get_byte()) < 0)
				goto bad_frame;
			if (c == 0x03 || c == 0x17) {
				incomplete = (c == 0x17);
				break;
			}
		}
		*p++ = c;
		check ^= c;
	}
	/* Append a sentry '\0' at the end of the buffer, for the convenience
	   of C programmers */
	*p = '\0';
	answer_len = p - answer;
	check ^= c;
	c = get_byte();
	if (c != check)
		return -1;
	if (answer[2] + (answer[3]<<8) != answer_len - 4)
		return -1;
	/* Return 0 for the last packet, 1 otherwise */
	return incomplete;
}

int cmd (int len, unsigned char *data, FILE *fd)
{
	int c, retry;
	int timeout = 1;

	/* Some commands require larger timeouts */
	switch (data[1]) {
	  case 0x27:	/* Take picture */
	  case 0x34:	/* Recharge the flash */
	  case 0x64:	/* Take preview */
	    timeout = 12;
	    break;
	  case 0x0b:	/* Count pictures */
	  case 0x19:    /* Erase a picture */
	    timeout = 2;
	    break;
	}

	retry = 0;
send_cmd:
	send_packet(len, data, 1);
	wait_for_input(timeout);
wait_ack:
	c = get_byte();
	if (c == 0x06)
		goto send_ok;
	if (++retry == 3) {
		fprintf(stderr,
		  "Cannot issue command %02x, aborting.\n", data[1]);
		exit(1);
	}
	if (c == 0x15)
		goto send_cmd;
	/* Garbled answer? Throw it away and ask for resend */
	while (get_byte() >= 0)
		continue;
	put_byte(0x15);
	goto wait_ack;

send_ok:
	retry = 0;
	wait_for_input(timeout);
	do {
	  c = read_packet();
	  if (c < 0) {
	    if (++retry == 3) {
		fprintf(stderr,
		  "Cannot receive answer (cmd=%02x), aborting.\n", data[1]);
		exit(1);
	    }
	    put_byte(0x15);
	    continue;
	  }
	  if (c && interrupted) {
	    /* Not the last packet */
	    fprintf(stderr, "\nInterrupted!\n");
	    exit(1);
	  }
	  put_byte(0x06);
	  if (fd != NULL)
	    fwrite(answer+4, 1, answer_len-4, fd);
	} while(c);

	/* Success */
	return 0;
}

int cmd0 (int c0, int c1, FILE *fd)
{
	unsigned char b[4];

	b[0] = c0; b[1] = c1;
	b[2] = b[3] = 0;
	return cmd(4, b, fd);
}

int cmd1 (int c0, int c1, int arg, FILE *fd)
{
	unsigned char b[5];

	b[0] = c0; b[1] = c1;
	b[2] =  1; b[3] =  0;
	b[4] = arg;
	return cmd(5, b, fd);
}

int cmd2 (int c0, int c1, int arg, FILE *fd)
{
	unsigned char b[6];

	b[0] = c0; b[1] = c1;
	b[2] =  2; b[3] =  0;
	b[4] = arg; b[5] = arg>>8;
	return cmd(6, b, fd);
}

char* dc_version_info (void)
{
	cmd0 (0, 0x09, 0);
	return answer+4;
}

char* dc_camera_type (void)
{
	cmd0 (0, 0x29, 0);
	return answer+4;
}

char* dc_camera_id (void)
{
	cmd0 (0, 0x80, 0);
	return answer+4;
}

int dc_set_camera_id (const char *id)
{
	unsigned char b[14];
	int n = strlen(id);

	if (n > 10)
		n = 10;
	b[0] = 0;
	b[1] = 0x82;
	b[2] = n;
	b[3] = 0;
	memcpy(b+4, id, n);
	return cmd(n+4, b, 0);
}

char* dc_get_date (void)
{
	char *fmtdate = answer+50;
	
	cmd0 (0, 0x84, 0);
	strcpy(fmtdate, "YYYY/MM/DD HH:MM:SS");
	memcpy(fmtdate,    answer+4,   4);	/* year */
	memcpy(fmtdate+5,  answer+8,   2);	/* month */
	memcpy(fmtdate+8,  answer+10,  2);	/* day */
	memcpy(fmtdate+11, answer+12,  2);	/* hour */
	memcpy(fmtdate+14, answer+14,  2);	/* minutes */
	memcpy(fmtdate+17, answer+16,  2);	/* seconds */

	return fmtdate;
}

int dc_set_date (const char *date)
{
	unsigned char b[18];
	int n = strlen(date);

	if (n > 14)
		n = 14;
	b[0] = 0;
	b[1] = 0x86;
	b[2] = n;
	b[3] = 0;
	memcpy(b+4, date, n);
	return cmd(n+4, b, 0);
}

int dc_get_flash_mode (void)
{
	cmd0 (0, 0x30, 0);
	return answer[4];
}

int dc_set_flash_mode (int mode)
{
	cmd1 (0, 0x32, mode, 0);
	return answer[4];
}

int dc_nb_pictures (void)
{
	cmd0 (0, 0x0b, 0);
	return answer[4] + (answer[5]<<8);
}

char *dc_picture_name (int i)
{
	cmd2 (0, 0x0a, i, 0);
	return answer+4;
}

int dc_picture_size (int i)
{
	cmd2 (0, 0x17, i, 0);
	return answer[4] + (answer[5] << 8) + (answer[6] << 16) + (answer[7] << 24);
}

int charge_flash (int amount)
{
	cmd2 (0, 0x34, amount, 0);
	return answer[4];
}

int take_picture (void)
{
	cmd0 (0, 0x27, 0);
	return answer[4] + (answer[5] << 8) + (answer[6] << 16) + (answer[7] << 24);
}

int del_frame (int i)
{
	cmd2 (0, 0x19, i, 0);
	return answer[4];
}

void get_command_list (int ds7_compat)
{
	int i;

	memset(has_cmd, 0, 256);
	if (ds7_compat) {
		/*
		 * The DS-7 doesn't have the 4C command to query capabilities;
		 * therefore, we assume a very minimal command set.
		 */
#if 0
		/*
		 * If you are daring, uncomment these lines; what follows
		 * is what I conjecture to be the actual capability set.
		 * Read mx700-commands.html for details. I'd like to have
		 * your feedback about this.
		 */
		static unsigned char ds7_cmds[] = {
			0x00, 0x02, 0x07, 0x09, 0x0a, 0x0b, 0x0c, 0x0e,
			0x0f, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x27,
			0x29, 0x30, 0x32, 0x34 };
		for (i = 0; i < sizeof(ds7_cmds); i++)
			has_cmd[ds7_cmds[i]] = 1;
#endif
		return;
	}
	cmd0 (0, 0x4c, 0);
	for (i = 4; i < answer_len; i++)
		has_cmd[answer[i]] = 1;
	if (list_command_set) {
		fprintf(stderr, "Supported commands:");
		for (i = 4; i < answer_len; i++)
			fprintf(stderr, "%s%02x", (i%16==4 ? "\n\t" : " "), answer[i]);
		fprintf(stderr, "\n");
	}
}

void get_picture_list (void)
{
	int i, n_off;
	char *name;
	struct stat st;

	pictures = dc_nb_pictures();
	maxnum = 100;
	free(pinfo);
	pinfo = calloc(pictures+1, sizeof(struct pict_info));
	for (i = 1; i <= pictures; i++) {
		name = strdup(dc_picture_name(i));
		pinfo[i].name = name;
		/*
		 * To find the picture number, go to the first digit. According to
		 * recent Exif specs, n_off can be either 3 or 4.
		 */
		n_off = strcspn(name, "0123456789");
		if ((pinfo[i].number = atoi(name+n_off)) > maxnum)
			maxnum = pinfo[i].number;
		pinfo[i].size = dc_picture_size(i);
		pinfo[i].ondisk = !stat(name, &st);
	}
}

void list_pictures (void)
{
	int i;
	struct pict_info* pi;
	char ex;

	for (i = 1; i <= pictures; i++) {
		pi = &pinfo[i];
		ex = pi->ondisk ? '*' : ' ';
		printf("%3d%c  %12s  %7d\n", i, ex, pi->name, pi->size);
	}
}

void close_connection (void)
{
	put_byte(0x04);
	tcdrain(devfd);
	usleep(50000);
}

void reset_serial (void)
{
	if (devfd >= 0) {
		close_connection();
		tcsetattr(devfd, TCSANOW, &oldt);
		remove(TMP_PIC_FILE);
	}
	devfd = -1;
}

void init_serial (const char *devname)
{
	devfd = open(devname, O_RDWR|O_NOCTTY);
	if (devfd < 0) {
		perror("Cannot open device");
		exit(1);
	}
	if (tcgetattr(devfd, &oldt) < 0) {
		perror("tcgetattr");
		exit(1);
	}
	newt = oldt;
	newt.c_iflag |= (PARMRK|INPCK);
	newt.c_iflag &= ~(BRKINT|IGNBRK|IGNPAR|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IXOFF);
	newt.c_oflag &= ~(OPOST);
	newt.c_cflag |= (CLOCAL|CREAD|CS8|PARENB);
	newt.c_cflag &= ~(CSTOPB|HUPCL|PARODD);
	newt.c_lflag &= ~(ECHO|ECHOE|ECHOK|ECHONL|ICANON|ISIG|NOFLSH|TOSTOP);
	newt.c_cc[VMIN] = 0;
	newt.c_cc[VTIME] = 1;
	cfsetispeed(&newt, B9600);
	cfsetispeed(&newt, B9600);
	if (tcsetattr(devfd, TCSANOW, &newt) < 0) {
		perror("tcsetattr");
		exit(1);
	}
	atexit(reset_serial);
	attention();
}

void set_baudrate (void)
{
	struct baudrate_info *bi;
	int error;
	int debug = (desired_speed >= 0);

	for (bi = brinfo; bi->number; bi++) {
		/* Speed autodetection or not ? */
		if (desired_speed > 0 && desired_speed != bi->speed)
			continue;
		if (debug)
			fprintf(stderr, "set_baudrate: trying %6d bps... ", bi->speed);
		cmd1(1, 7, bi->number, 0);
		error = answer[4];
		if (debug) {
			if (error) fprintf(stderr, "not ");
			fprintf(stderr, "supported\n");
		}
		if (error)
			continue;
		/* This speed should be supported. Let's see. */
		close_connection();
		cfsetispeed(&newt, bi->posix_speed);
		cfsetospeed(&newt, bi->posix_speed);
		tcsetattr(devfd, TCSANOW, &newt);
		attention();
		if (debug)
			fprintf(stderr, "set_baudrate: new speed is %d bps\n", bi->speed);
		return;
	}
	fprintf(stderr, "set_baudrate: still at 9600 bps\n");
}

void download_picture(int n)
{
	FILE *fd;
	char *name = pinfo[n].name;
	int size = pinfo[n].size;
	struct stat st;
	struct tms stms;
	clock_t t1, t2;

	printf("%3d   %12s  ", n, name); fflush(stdout);
	fd = fopen(TMP_PIC_FILE, "w");
	if (fd == NULL) {
		perror("Cannot create picture file");
		exit(1);
	}
	t1 = times(&stms);
	cmd2(0, 0x02, n, fd);
	t2 = times(&stms);
	if (t1==t2) t2++; /* paranoia */
	printf("%3d seconds, ", (int)(t2-t1) / CLK_TCK);
	printf("%4d bytes/s\n", size * CLK_TCK / (int)(t2-t1));
	fclose(fd);
	if (stat(TMP_PIC_FILE, &st) < 0 || st.st_size != size) {
		/* Truncated file */
		fprintf(stderr, "Short picture file -- disk full or quota exceeded\n");
		exit(1);
	}
	if (rename(TMP_PIC_FILE, name) < 0) {
		perror("Cannot rename file");
		exit(1);
	}
	pinfo[n].transferred = 1;
}

void download_range (int start, int end, int picnums, int force)
{
	int i, num;
	struct pict_info *pi;

	for (i = 1; i <= pictures; i++) {
		pi = &pinfo[i];
		if (!force && pi->ondisk)
			continue;
		num = picnums ? pi->number : i;
		if (num < start || num > end)
			continue;
		download_picture(i);
	}
}

int dc_free_memory (void)
{
	cmd0 (0, 0x1B, 0);
	return answer[5] + (answer[6]<<8) + (answer[7]<<16) + (answer[8]<<24);
}

int delete_pic (const char *picname)
{
	int i, ret;

	for (i = 1; i <= pictures; i++)
	  if (!strcmp(pinfo[i].name, picname)) {
	    if ((ret = del_frame(i)) == 0)
	      get_picture_list();
	    return ret;
	  }
	return -1;
}

char* auto_rename (void)
{
	static char buffer[13];
	
	if (maxnum < 99999)
		maxnum++;

	sprintf(buffer, "DSC%05d.JPG", maxnum);
	return buffer;
}

int upload_pic (const char *picname)
{
	unsigned char buffer[516];
	const char *p;
	struct stat st;
	FILE *fd;
	int c, last, len, free_space;

	fd = fopen(picname, "r");
	if (fd == NULL) {
		fprintf(stderr, "Cannot open file %s for upload\n", picname);
		return 0;
	}
	if (fstat(fileno(fd), &st) < 0) {
		perror("fstat");
		return 0;
	}
	free_space = dc_free_memory();
	fprintf(stderr, "Uploading %s (size %d, available %d bytes)\n",
		picname, (int) st.st_size, free_space);
	if (st.st_size > free_space) {
		fprintf(stderr, "  not enough space\n");
		return 0;
	}
	if ((p = strrchr(picname, '/')) != NULL)
		picname = p+1;
	if (strlen(picname) != 12 || memcmp(picname,"DSC",3) || memcmp(picname+8,".JPG",4)) {
		picname = auto_rename();
		fprintf(stderr, "  file renamed %s\n", picname);
	}
	buffer[0] = 0;
	buffer[1] = 0x0F;
	buffer[2] = 12;
	buffer[3] = 0;
	memcpy(buffer+4, picname, 12);
	cmd(16, buffer, 0);
	if (answer[4] != 0) {
		fprintf(stderr, "  rejected by the camera\n");
		return 0;
	}
	buffer[1] = 0x0E;
	while(1) {
		len = fread(buffer+4, 1, 512, fd);
		if (!len) break;
		buffer[2] = len;
		buffer[3] = (len>>8);
		last = 1;
		if ((c = getc(fd)) != EOF) {
			last = 0;
			ungetc(c, fd);
		}
		if (!last && interrupted) {
			fprintf(stderr, "Interrupted!\n");
			exit(1);
		}
again:
		send_packet(4+len, buffer, last);
		wait_for_input(1);
		if (get_byte() == 0x15)
			goto again;
	}
	fclose(fd);
	fprintf(stderr, "  looks ok\n");
	return 1;
}

const char *Usage = "\
Usage: fujiplay [OPTIONS] PICTURES...          (download)\r\n\
                          charge NUMBER        (recharge the flash)\r\n\
                          shoot                (take picture)\r\n\
                          preview              (preview to standard output)\r\n\
                          upload FILES...\r\n\
                          delete FILES...\r\n\
                          setid STRING         (set camera ID)\r\n\
                          setflash MODE        (0=Off, 1=On, 2=Strobe, 3=Auto)\r\n\
                          setdate gmt|local|YYYYMMDDHHMMSS\r\n\
Options:\r\n\
  -B NUMBER	Set baudrate (115200, 57600, 38400, 19200, 9600 or 0)\r\n\
  -D DEVICE	Select another device file (default is /dev/fujifilm)\r\n\
  -L		List command set\r\n\
  -7		DS-7 compatibility mode (experimental)\r\n\
  -d		Delete pictures after successful download\r\n\
  -f		Force (overwrite existing files)\r\n\
  -p		Assume picture numbers instead of frame numbers\r\n\
  -h		Display this help message\r\n\
  -v		Version information\r\n\
Pictures:\r\n\
  all		All pictures\r\n\
  last		Last picture\r\n\
  4		Only picture 4\r\n\
  2-10		Pictures between 2 and 10\r\n\
Files:\r\n\
  DSCxxxxx.JPG	Files to delete or to upload into the camera\r\n\
";

const char *Copyright = "\r\n\
Fujiplay, $Id: fujiplay.c,v 1.33 1999/02/22 14:13:01 bousch Exp $\r\n\
Written by Thierry Bousch <bousch@topo.math.u-psud.fr>\r\n\
Public domain. Absolutely no warranty.\r\n\
";

static void sigint_handler (int sig)
{
	interrupted = 1;
}

int main (int argc, char **argv)
{
	extern char *optarg;
	extern int optind, opterr, optopt;

	int i, c, deleted;
	int ds7_compat=0, force=0, picnums=0, delete_after=0;
	struct sigaction s2act;
	time_t now;
	struct tm *ptm;
	char datebuff[50];
	char *devname = DEFAULT_DEVICE;
	char *dash, *arg;

	s2act.sa_handler = sigint_handler;
	sigemptyset(&s2act.sa_mask); s2act.sa_flags = 0;
	sigaction(SIGINT, &s2act, NULL);

	/* Command line parsing */
	while ((c = getopt(argc,argv,"B:D:L7dfhpv")) != EOF)
	switch(c) {
		case 'B':
			desired_speed = atoi(optarg);
			break;
		case 'D':
			devname = optarg;
			break;
		case 'L':
			list_command_set = 1;
			break;
		case '7':
			ds7_compat = 1;
			break;
		case 'd':
			delete_after = 1;
			break;
		case 'f':
			force = 1;
			break;
		case 'p':
			picnums = 1;
			break;
		case 'h':
			printf(Usage);
			return 0;
		case 'v':
			printf(Copyright);
			return 0;
		default:
			fprintf(stderr, Usage);
			return 1;
	}

	init_serial(devname);
	set_baudrate();
	get_command_list(ds7_compat);
	get_picture_list();

	if (optind == argc) {
		if (has_cmd[0x09])
			fprintf(stderr, "Version info: %s\n", dc_version_info());
		if (has_cmd[0x29])
			fprintf(stderr, "Camera type : %s\n", dc_camera_type());
		if (has_cmd[0x84])
			fprintf(stderr, "Camera date : %s\n", dc_get_date());
		if (has_cmd[0x80])
			fprintf(stderr, "Camera ID   : %s\n", dc_camera_id());
		if (has_cmd[0x1B])
			fprintf(stderr, "Free memory : %d kb\n", dc_free_memory() >> 10);
		if (has_cmd[0x30]) {
			int flashmode;
			char *tmode;
			flashmode = dc_get_flash_mode();
			switch(flashmode) {
				case 0:  tmode = "off";    break;
				case 1:  tmode = "on";     break;
				case 2:  tmode = "strobe"; break;
				case 3:  tmode = "auto";   break;
				default: tmode = "unknown";
			}
			fprintf(stderr, "Flash mode  : %d (%s)\n", flashmode, tmode);
		}
		list_pictures();
		return 0;
	}
	if (!strcmp(argv[optind], "charge") && optind+1 < argc) {
		if (!has_cmd[0x34]) {
			fprintf(stderr, "Cannot charge flash (unsupported command)\n");
			return 1;
		}
		arg = argv[optind+1];
		charge_flash(atoi(arg));
		return 0;
	}
	if (!strcmp(argv[optind], "shoot")) {
		if (!has_cmd[0x27]) {
			fprintf(stderr, "Cannot shoot (unsupported command)\n");
			return 1;
		}
		c = take_picture();
		printf("%3d   %12s  %7d\n", c, dc_picture_name(c), dc_picture_size(c));
		return 0;
	}
	if (!strcmp(argv[optind], "preview")) {
		if (!has_cmd[0x62] || !has_cmd[0x64]) {
			fprintf(stderr, "Cannot preview (unsupported command)\n");
			return 1;
		}
		cmd0(0, 0x64, 0);
		cmd0(0, 0x62, stdout);
		return 0;
	}
	if (!strcmp(argv[optind], "setid") && optind+1 < argc) {
		if (!has_cmd[0x82]) {
			fprintf(stderr, "Cannot set camera ID (unsupported command)\n");
			return 1;
		}
		arg = argv[optind+1];
		dc_set_camera_id(arg);
		return 0;
	}
	if (!strcmp(argv[optind], "setdate") && optind+1 < argc) {
		if (!has_cmd[0x86]) {
			fprintf(stderr, "Cannot set date (unsupported command)\n");
			return 1;
		}
		arg = argv[optind+1];
		now = time(0);
		if (!strcmp(arg, "gmt") || !strcmp(arg, "utc")) {
			ptm = gmtime(&now);
			goto set_date_from_tm;
		}
		if (!strcmp(arg, "local")) {
			ptm = localtime(&now);
			goto set_date_from_tm;
		}
		goto set_date_from_arg;
set_date_from_tm:
		sprintf(datebuff, "%04d%02d%02d%02d%02d%02d",
			1900+ptm->tm_year, 1+ptm->tm_mon, ptm->tm_mday,
			ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
		fprintf(stderr, "Current date (%s) is %s\n", arg, datebuff);
		arg = datebuff;
set_date_from_arg:
		dc_set_date(arg);
		return 0;
	}
	if (!strcmp(argv[optind], "setflash") && optind+1 < argc) {
		if (!has_cmd[0x32]) {
			fprintf(stderr, "Cannot set flash mode (unsupported command)\n");
			return 1;
		}
		arg = argv[optind+1];
		dc_set_flash_mode(atoi(arg));
		return 0;
	}
	if (!strcmp(argv[optind], "delete")) {
		/* Always supported, I guess */
		for (i = optind+1; i < argc; i++)
		    delete_pic(argv[i]);
		return 0;
	}
	if (!strcmp(argv[optind], "upload")) {
		if (!has_cmd[0x0e] || !has_cmd[0x0f]) {
			fprintf(stderr, "Cannot upload pictures (unsupported command)\n");
			return 1;
		}
		for (i = optind+1; i < argc; i++)
		    upload_pic(argv[i]);
		return 0;
	}
	printf("Loading pictures:\n");
	for (i = optind; i < argc; i++) {
		arg = argv[i];
		dash = strchr(arg, '-');
		if (!strcmp(arg, "all"))
		  download_range(0, 99999, 0, force);
		else if (!strcmp(arg, "last"))
		  download_range(maxnum, maxnum, 1, force);
		else if (dash)
		  download_range(atoi(arg), atoi(dash+1), picnums, force);
		else
		  download_range(atoi(arg), atoi(arg), picnums, force);
	}
	if (delete_after) {
		sync();
		deleted = 0;
		for (c = pictures; c > 0; c--)
			if (pinfo[c].transferred)
				deleted += !del_frame(c);
		printf("Deleted %d picture(s).\n", deleted);
	}
	return 0;
}
