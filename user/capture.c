/*******************************************************************************
 * capture.c                                                                   *
 *                                                                             *
 * USB Driver for Somagic EasyCAP DC60                                         *
 * USB ID 1c88:0007                                                            *
 *                                                                             *
 * Initializes the Somagic EasyCAP DC60 registers and performs image capture.  *
 * *****************************************************************************
 *
 * Copyright 2011, 2012 Tony Brown, Jeffry Johnston, Michal Demin	
 *
 * This file is part of somagic_dc60
 * http://code.google.com/p/easycap-somagic-linux/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * Usage (run as root):
 * init
 * capture -p 2> /dev/null | mplayer - -vf screenshot -demuxer rawvideo -rawvideo "w=720:h=576:format=uyvy:fps=25"
 * capture -n 2> /dev/null | mplayer - -vf screenshot -demuxer rawvideo -rawvideo "ntsc:format=uyvy:fps=30000/1001"
 */

/* This file was originally generated with usbsnoop2libusb.pl from a usbsnoop log file. */
/* Latest version of the script should be in http://iki.fi/lindi/usb/usbsnoop2libusb.pl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <libusb-1.0/libusb.h>
#include <execinfo.h>
#include <unistd.h>
#include <getopt.h>

#define VERSION "1.0"
#define VENDOR 0x1c88
#define PRODUCT 0x003c
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/* Control the number of concurrent ISO transfers we have running */
static const int NUM_ISO_TRANSFERS = 4;

int frames_generated = 0;
int stop_sending_requests = 0;
int pending_requests = 0;

struct libusb_device_handle *devh;

enum tv_standards {
	PAL,
	NTSC
};

enum input_types {
	CVBS,
	SVIDEO
};

/* Options */
/* Control the number of frames to generate: -1 = unlimited (default) */
int frame_count = -1;

/* Television standard: PAL (default) or NTSC */
int tv_standard = PAL;

/* Input select: CVBS/composite (default) or SVIDEO */
int input_type = CVBS;

/* Luminance mode (CVBS only): 0 = 4.1 MHz, 1 = 3.8 MHz, 2 = 2.6 MHz, 3 = 2.9 MHz */
int luminance_mode = 0;

/* Luminance prefilter: 0 = bypassed, 1 = active */
int luminance_prefilter = 0;

/* Hue phase in degrees: -128 to 127 (-180 to 178.59375), increments of 1.40625 degrees */
uint8_t hue = 0;

/* Chrominance saturation: -128 to 127 (1.984375 to -2.000000), increments of 0.015625 */
uint8_t saturation = 64;

/* Luminance contrast: -128 to 127 (1.984375 to -2.000000), increments of 0.015625 */
uint8_t contrast = 64;

/* Luminance brightness: 0 to 255 */
uint8_t brightness = 128;

void release_usb_device(int ret)
{
	fprintf(stderr, "Emergency exit\n");
	ret = libusb_release_interface(devh, 0);
	if (!ret) {
		perror("Failed to release interface");
	}
	libusb_close(devh);
	libusb_exit(NULL);
	exit(1);
}

struct libusb_device *find_device(int vendor, int product)
{
	struct libusb_device **list;
	struct libusb_device *dev = NULL;
	struct libusb_device_descriptor descriptor;
	int i;
	ssize_t count;
	count = libusb_get_device_list(NULL, &list);
	for (i = 0; i < count; i++) {
		struct libusb_device *item = list[i];
		libusb_get_device_descriptor(item, &descriptor);
		if (descriptor.idVendor == vendor && descriptor.idProduct == product) {
			dev = item;
		} else {
			libusb_unref_device(item);
		}
	}
	libusb_free_device_list(list, 0);
	return dev;
}

void print_bytes(unsigned char *bytes, int len)
{
	int i;
	if (len > 0) {
		for (i = 0; i < len; i++) {
			fprintf(stderr, "%02x ", (int)bytes[i]);
		}
		fprintf(stderr, "\"");
		for (i = 0; i < len; i++) {
			fprintf(stderr, "%c", isprint(bytes[i]) ? bytes[i] : '.');
		}
		fprintf(stderr, "\"");
	}
}

void print_bytes_only(char *bytes, int len)
{
	int i;
	if (len > 0) {
		for (i = 0; i < len; i++) {
			if (i % 32 == 0) {
				fprintf(stderr, "\n%04x\t ", i);
			}
			fprintf(stderr, "%02x ", (int)((unsigned char)bytes[i]));
			/* if ((i + 1) % 16 == 0) {	 fprintf(stderr, "\n"); } */
		}
	}
}

void trace()
{
	void *array[10];
	size_t size;

	/* get void*'s for all entries on the stack */
	size = backtrace(array, 10);

	/* print out all the frames */
	backtrace_symbols_fd(array, size, 1);
	exit(1);
}

enum sync_state {
	HSYNC,
	SYNCZ1,
	SYNCZ2,
	SYNCAV,
};

struct video_state_t {
	uint16_t line;
	uint16_t col;

	enum sync_state state;

	uint8_t field;
	uint8_t blank;
};

static struct video_state_t vs = { .line = 0, .col = 0, .state = 0, .field = 0, .blank = 0};

unsigned char frame[720 * 2 * 627 * 2] = { 0 };

static void put_data(struct video_state_t *vs, uint8_t c)
{
	int line_pos;

	line_pos = (2 * vs->line + vs->field) * (720 * 2) + vs->col;
	vs->col ++;

	/* sanity check */
	if (vs->col > 720 * 2)
		vs->col = 720 * 2;

	frame[line_pos] = c;
}

static void process(struct video_state_t *vs, uint8_t c)
{
	int lines_per_field = (tv_standard == PAL ? 288 : 240);

	if (vs->state == HSYNC) {
		if (c == 0xff) {
			vs->state++;
		} else {
			put_data(vs, c);
		}
	} else if (vs->state == SYNCZ1) {
		if (c == 0x00) {
			vs->state++;
		} else {
			vs->state = HSYNC;

			put_data(vs, 0xff);
			put_data(vs, c);
		}
	} else if (vs->state == SYNCZ2) {
		if (c == 0x00) {
			vs->state++;
		} else {
			vs->state = HSYNC;

			put_data(vs, 0xff);
			put_data(vs, 0x00);
			put_data(vs, c);
		}
	} else if (vs->state == SYNCAV) {
		vs->state = HSYNC;
		if (c == 0x00) {
			/* slice id */
			return;
		}

		if (c & 0x10) {
			/* EAV (end of active data) */
			if (!vs->blank) {
				vs->line++;
				vs->col = 0;
				if (vs->line > 625) vs->line = 625; /* sanity check */
			}
		} else {
			int field_edge;
			int blank_edge;

			/* SAV (start of active data) */
			/*
				* F (field bit) = Bit 6 (mask 0x40).
				* 0: first field, 1: 2nd field.
				*
				* V (vertical blanking bit) = Bit 5 (mask 0x20).
				* 0: in VBI, 1: in active video.
				*/
			field_edge = vs->field;
			blank_edge = vs->blank;

			vs->field = (c & 0x40) ? 1 : 0;
			vs->blank = (c & 0x20) ? 1 : 0;

			field_edge = vs->field ^ field_edge;
			blank_edge = vs->blank ^ blank_edge;

			if (vs->field == 0 && field_edge) {
				if (frames_generated < frame_count || frame_count == -1) {
					write(1, frame, 720 * 2 * lines_per_field * 2);
					frames_generated++;
				}
				
				if (frames_generated >= frame_count && frame_count != -1) {
					stop_sending_requests = 1;
				}
			}

			if (vs->blank == 0 && blank_edge) {
				vs->line = 0;
				vs->col = 0;
			}
		}
	}
}

void gotdata(struct libusb_transfer *tfr)
{
	int ret;
	int num = tfr->num_iso_packets;
	int i;

	pending_requests--;

	for (i = 0; i < num; i++) {
		unsigned char *data = libusb_get_iso_packet_buffer_simple(tfr, i);
		int length = tfr->iso_packet_desc[i].actual_length;
		int pos = 0;
		int k;

		while (pos < length) {
			/*
			 * Within each packet of the transfer, the data is divided into blocks of 0x400 bytes
			 * beginning with [0xaa 0xaa 0x00 0x00].
			 * Check for this signature and process each block of data individually.
			 */
			if (data[pos] == 0xaa && data[pos + 1] == 0xaa && data[pos + 2] == 0x00 && data[pos + 3] == 0x00) {
				/* process the received data, excluding the 4 marker bytes */
				for (k = 0; k < 0x400 - 4; k++) {
					process(&vs, data[k + 4 + pos]);
				}
			} else {
				fprintf(stderr, "Unexpected block, expected [aa aa 00 00] found [%02x %02x %02x %02x]\n", data[pos], data[pos + 1], data[pos + 2], data[pos + 3]);
			}
			pos += 0x400;
		}
	}

	if (!stop_sending_requests) {
		ret = libusb_submit_transfer(tfr);
		if (ret != 0) {
			fprintf(stderr, "libusb_submit_transfer failed with error %d\n", ret);
			exit(1);
		}
		pending_requests++;
	}
}

uint8_t somagic_read_reg(uint16_t reg)
{
	int ret;
	uint8_t buf[13];
	memcpy(buf, "\x0b\x00\x20\x82\x01\x30\x80\xFF", 8);

	buf[5] = reg >> 8;
	buf[6] = reg & 0xff;

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 8, 1000);
	if (ret != 8) {
		fprintf(stderr, "read_reg msg returned %d, bytes: ", ret);
		print_bytes(buf, ret);
		fprintf(stderr, "\n");
	}

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 13, 1000);
	if (ret != 13) {
		fprintf(stderr, "read_reg control msg returned %d, bytes: ", ret);
		print_bytes(buf, ret);
		fprintf(stderr, "\n");
	}

	return buf[7];
}

static int somagic_write_reg(uint16_t reg, uint8_t val)
{
	int ret;
	uint8_t buf[8];

	memcpy(buf, "\x0b\x00\x00\x82\x01\x00\x3a\x00", 8);
	buf[5] = reg >> 8;
	buf[6] = reg & 0xff;
	buf[7] = val;

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 8, 1000);
	if (ret != 8) {
		fprintf(stderr, "write reg control msg returned %d, bytes: ", ret);
		print_bytes(buf, ret);
		fprintf(stderr, "\n");
	}

	return ret;
}

static uint8_t somagic_read_i2c(uint8_t dev_addr, uint8_t reg)
{
	int ret;
	uint8_t buf[13];

	memcpy(buf, "\x0b\x4a\x84\x00\x01\x10\x00\x00\x00\x00\x00\x00\x00", 13);

	buf[1] = dev_addr;
	buf[5] = reg;

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 13, 1000);
	fprintf(stderr, "-> i2c_read msg returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	usleep(18 * 1000);

	memcpy(buf, "\x0b\x4a\xa0\x00\x01\x00\xff\xff\xff\xff\xff\xff\xff", 13);

	buf[1] = dev_addr;

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 13, 1000);
	fprintf(stderr, "-> i2c_read msg returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");

	memset(buf, 0xff, 0x000000d);
	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x000000b, 0x0000000, buf, 13, 1000);
	fprintf(stderr, "<- i2c_read msg returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	usleep(11 * 1000);

	return buf[5];
}

static int somagic_write_i2c(uint8_t dev_addr, uint8_t reg, uint8_t val)
{
	int ret;
	uint8_t buf[8];

	memcpy(buf, "\x0b\x4a\xc0\x01\x01\x01\x08\xf4", 8);

	buf[1] = dev_addr;
	buf[5] = reg;
	buf[6] = val;

	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x000000b, 0x0000000, buf, 8, 1000);
	if (ret != 8) {
		fprintf(stderr, "write_i2c returned %d, bytes: ", ret);
		print_bytes(buf, ret);
		fprintf(stderr, "\n");
	}

	return ret;
}

void version()
{
	fprintf(stderr, "capture "VERSION"\n");
	fprintf(stderr, "Copyright 2011, 2012 Tony Brown, Jeffry Johnston, Michal Demin\n");
	fprintf(stderr, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>.\n");
	fprintf(stderr, "This is free software: you are free to change and redistribute it.\n");
	fprintf(stderr, "There is NO WARRANTY, to the extent permitted by law.\n");
}

void usage()
{
	fprintf(stderr, "Usage: capture [options]\n");
	fprintf(stderr, "  -c, --cvbs              Use CVBS (composite) input (default)\n");
	fprintf(stderr, "  -B, --brightness=VALUE  Luminance brightness control,\n");
	fprintf(stderr, "                          0 to 255 (default: 128)\n");
	fprintf(stderr, "                          Value  Brightness\n");
	fprintf(stderr, "                            255  Bright\n");
	fprintf(stderr, "                            128  ITU level (default)\n");
	fprintf(stderr, "                              0  Dark\n");
	fprintf(stderr, "  -C, --contrast=VALUE    Luminance contrast control,\n");
	fprintf(stderr, "                          -128 to 127 (default: 64)\n");
	fprintf(stderr, "                          Value  Contrast\n");
	fprintf(stderr, "                            127   1.984375\n");
	fprintf(stderr, "                             71   1.109375 (ITU level)\n");
	fprintf(stderr, "                             64   1.000000 (default)\n");
	fprintf(stderr, "                              1   0.015625\n");
	fprintf(stderr, "                              0   0.000000 (luminance off)\n");
	fprintf(stderr, "                            -64  -1.000000 (inverse)\n");
	fprintf(stderr, "                           -128  -2.000000 (inverse)\n");
	fprintf(stderr, "  -f, --frames=COUNT      Number of frames to generate,\n");
	fprintf(stderr, "                          -1 for unlimited (default: -1)\n");
	fprintf(stderr, "  -H, --hue=VALUE         Hue phase in degrees, -128 to 127 (default: 0),\n");
	fprintf(stderr, "                          Value  Phase\n");
	fprintf(stderr, "                           -128  -180.00000\n");
	fprintf(stderr, "                              0     0.00000\n");
	fprintf(stderr, "                              1     1.40635\n");
	fprintf(stderr, "                            127   178.59375\n");
	fprintf(stderr, "  -l, --luminance=MODE    CVBS luminance mode (default: 0)\n");
	fprintf(stderr, "                          Mode  Center Frequency\n");
	fprintf(stderr, "                             0  4.1 MHz (default)\n");
	fprintf(stderr, "                             1  3.8 MHz\n");
	fprintf(stderr, "                             2  2.6 MHz\n");
	fprintf(stderr, "                             3  2.9 MHz\n");
	fprintf(stderr, "  -L, --lum-prefilter     Activate luminance prefilter (default: bypassed)\n");
	fprintf(stderr, "  -n, --ntsc              Television standard is 60Hz NTSC\n");
	fprintf(stderr, "  -p, --pal               Television standard is 50Hz PAL (default)\n");
	fprintf(stderr, "  -S, --saturation=VALUE  Chrominance saturation control,\n");
	fprintf(stderr, "                          -128 to 127 (default: 64)\n");
	fprintf(stderr, "                          Value  Saturation\n");
	fprintf(stderr, "                            127   1.984375\n");
	fprintf(stderr, "                             64   1.000000 (ITU level, default)\n");
	fprintf(stderr, "                              1   0.015625\n");
	fprintf(stderr, "                              0   0.000000 (color off)\n");
	fprintf(stderr, "                            -64  -1.000000 (inverse)\n");
	fprintf(stderr, "                           -128  -2.000000 (inverse)\n");
	fprintf(stderr, "  -s, --s-video           Use S-VIDEO input\n");
	fprintf(stderr, "      --help              Display usage\n");
	fprintf(stderr, "      --version           Display version information\n");
}

int main(int argc, char **argv)
{
	int ret;
	int i = 0;
	uint8_t status;
	uint8_t work; 
	struct libusb_device *dev;

	/* buffer for control messages */
	unsigned char buf[65535];
	
	/* buffers and transfer pointers for isochronous data */
	struct libusb_transfer *tfr[NUM_ISO_TRANSFERS];
	unsigned char isobuf[NUM_ISO_TRANSFERS][64 * 3072];

	/* parsing */
	int c;
	int option_index = 0;
	static struct option long_options[] = {
		{"help", 0, 0, 0}, /* index 0 */
		{"version", 0, 0, 0}, /* index 1 */
		{"brightness", 1, 0, 'B'},
		{"cvbs", 0, 0, 'c'},
		{"contrast", 1, 0, 'C'},
		{"frame-count", 1, 0, 'f'},
		{"hue", 1, 0, 'H'},
		{"luminance", 1, 0, 'l'},
		{"lum-prefilter", 0, 0, 'L'},
		{"ntsc", 0, 0, 'n'},
		{"pal", 0, 0, 'p'},
		{"s-video", 0, 0, 's'},
		{"saturation", 1, 0, 'S'},
		{0, 0, 0, 0}
	};

	/* parse command line arguments */
	while (1) {
		c = getopt_long(argc, argv, "B:cC:f:H:l:LnpsS:", long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 0:
			switch (option_index) {	
			case 0: /* --help */
				usage();
				return 0;
			case 1: /* --version */
				version();
				return 0;
			default:
				usage();
				return 1;
			}
			break;
		case 'B':
			i = atoi(optarg);
			if (i < 0 || i > 255) {
				fprintf(stderr, "Invalid brightness value '%i', must be from 0 to 255\n", i);
				return 1;
			}
			brightness = i;
			break;
		case 'c':
			input_type = CVBS;
			break;
		case 'C':
			i = atoi(optarg);
			if (i < -128 || i > 127) {
				fprintf(stderr, "Invalid contrast value '%i', must be from -128 to 127\n", i);
				return 1;
			}
			contrast = (int8_t)i;
			break;
		case 'f':
			frame_count = atoi(optarg);
			break;
		case 'H':
			i = atoi(optarg);
			if (i < -128 || i > 127) {
				fprintf(stderr, "Invalid hue phase '%i', must be from -128 to 127\n", i);
				return 1;
			}
			hue = (int8_t)i;
			break;
		case 'l':
			luminance_mode = atoi(optarg);
			if (luminance_mode < 0 || luminance_mode > 3) {
				fprintf(stderr, "Invalid luminance mode '%i', must be from 0 to 3\n", luminance_mode);
				return 1;
			}
			break;
		case 'L':
			luminance_prefilter = 1;
			break;
		case 'n':
			tv_standard = NTSC;
			break;
		case 'p':
			tv_standard = PAL;
			break;
		case 's':
			input_type = SVIDEO;
			break;
		case 'S':
			i = atoi(optarg);
			if (i < -128 || i > 127) {
				fprintf(stderr, "Invalid saturation value '%i', must be from -128 to 127\n", i);
				return 1;
			}
			saturation = (int8_t)i;
			break;
		default:
			usage();
			return 1;
		}
	}
	if (optind < argc) {
		usage();
		return 1;
	}
	if (input_type == SVIDEO && luminance_mode != 0) {
		fprintf(stderr, "Luminance mode must be 0 for S-VIDEO\n");
		return 1;
	}

	libusb_init(NULL);
	libusb_set_debug(NULL, 0);

	dev = find_device(VENDOR, PRODUCT);
	if (!dev) {
		fprintf(stderr, "USB device %04x:%04x was not found.\n", VENDOR, PRODUCT);
		return 1;
	}

	ret = libusb_open(dev, &devh);
	if (!devh) {
		perror("Failed to open USB device");
		return 1;
	}
	libusb_unref_device(dev);
	
	signal(SIGTERM, release_usb_device);
	ret = libusb_claim_interface(devh, 0);
	if (ret != 0) {
		fprintf(stderr, "claim failed with error %d\n", ret);
		exit(1);
	}
	
	ret = libusb_set_interface_alt_setting(devh, 0, 0);
	if (ret != 0) {
		fprintf(stderr, "set_interface_alt_setting failed with error %d\n", ret);
		exit(1);
	}

	ret = libusb_get_descriptor(devh, 0x0000001, 0x0000000, buf, 0x0000012);
	fprintf(stderr, "1 get descriptor returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000009);
	fprintf(stderr, "2 get descriptor returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000042);
	fprintf(stderr, "3 get descriptor returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");

	ret = libusb_release_interface(devh, 0);
	if (ret != 0) {
		fprintf(stderr, "failed to release interface before set_configuration: %d\n", ret);
	}
	ret = libusb_set_configuration(devh, 0x0000001);
	fprintf(stderr, "4 set configuration returned %d\n", ret);
	ret = libusb_claim_interface(devh, 0);
	if (ret != 0) {
		fprintf(stderr, "claim after set_configuration failed with error %d\n", ret);
	}
	ret = libusb_set_interface_alt_setting(devh, 0, 0);
	fprintf(stderr, "4 set alternate setting returned %d\n", ret);
	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE + LIBUSB_ENDPOINT_IN, 0x0000001, 0x0000001, 0x0000000, buf, 2, 1000);
	fprintf(stderr, "5 control msg returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");

	somagic_write_reg(0x3a, 0x80);
	somagic_write_reg(0x3b, 0x00);

	/* Reset audio chip? */
	somagic_write_reg(0x34, 0x01);
	somagic_write_reg(0x35, 0x00);

	status = somagic_read_reg(0x3080);
	fprintf(stderr, "status is %02x\n", status);

	/* Reset audio chip? */
	somagic_write_reg(0x34, 0x11);
	somagic_write_reg(0x35, 0x11);

	/* SAAxxx: toggle reset of SAAxxx */
	somagic_write_reg(0x3b, 0x80);

	/* SAAxxx: bring from reset */
	somagic_write_reg(0x3b, 0x00);

	/* Subaddress 0x01, Horizontal Increment delay */
	/* Recommended position */
	somagic_write_i2c(0x4a, 0x01, 0x08);

	/* Subaddress 0x02, Analog input control 1 */
	if (input_type == CVBS) {
		/* Analog function select FUSE = Amplifier plus anti-alias filter bypassed */
		/* Update hysteresis for 9-bit gain = Off */
		/* Mode = 0, CVBS (automatic gain) from AI11 (pin 4) */
		somagic_write_i2c(0x4a, 0x02, 0xc0);
	} else {
		/* Analog function select FUSE = Amplifier plus anti-alias filter bypassed */
		/* Update hysteresis for 9-bit gain = Off */
		/* Mode = 7, Y (automatic gain) from AI12 (pin 7) + C (gain adjustable via GAI28 to GAI20) from AI22 (pin 1) */
		somagic_write_i2c(0x4a, 0x02, 0xc7);
	}

	/* Subaddress 0x03, Analog input control 2 */ 
	if (input_type == CVBS) {
		/* Static gain control channel 1 (GAI18), sign bit of gain control = 1 */
		/* Static gain control channel 2 (GAI28), sign bit of gain control = 1 */
		/* Gain control fix (GAFIX) = Automatic gain controlled by MODE3 to MODE0 */
		/* Automatic gain control integration (HOLDG) = AGC active */
		/* White peak off (WPOFF) = White peak off */ 
		/* AGC hold during vertical blanking period (VBSL) = Long vertical blanking (AGC disabled from start of pre-equalization pulses until start of active video (line 22 for 60 Hz, line 24 for 50 Hz) */
		/* Normal clamping if decoder is in unlocked state */
		somagic_write_i2c(0x4a, 0x03, 0x33); 
	} else {
		/* Static gain control channel 1 (GAI18), sign bit of gain control = 1 */
		/* Static gain control channel 2 (GAI28), sign bit of gain control = 0 */
		/* Gain control fix (GAFIX) = Automatic gain controlled by MODE3 to MODE0 */
		/* Automatic gain control integration (HOLDG) = AGC active */
		/* White peak off (WPOFF) = White peak off */ 
		/* AGC hold during vertical blanking period (VBSL) = Long vertical blanking (AGC disabled from start of pre-equalization pulses until start of active video (line 22 for 60 Hz, line 24 for 50 Hz) */
		/* Normal clamping if decoder is in unlocked state */
		somagic_write_i2c(0x4a, 0x03, 0x31); 
	}

	/* Subaddress 0x04, Gain control analog/Analog input control 3 (AICO3); static gain control channel 1 GAI1 */
	/* Gain (dB) = -3 (Note: Dependent on subaddress 0x03 GAI18 value) */
	somagic_write_i2c(0x4a, 0x04, 0x00);

	/* Subaddress 0x05, Gain control analog/Analog input control 4 (AICO4); static gain control channel 2 GAI2 */
	/* Gain (dB) = -3 (Note: Dependent on subaddress 0x03 GAI28 value) */
	somagic_write_i2c(0x4a, 0x05, 0x00);

	/* Subaddress 0x06, Horizontal sync start/begin */
	/* Delay time (step size = 8/LLC) = Recommended value for raw data type */
	somagic_write_i2c(0x4a, 0x06, 0xe9);

	/* Subaddress 0x07, Horizontal sync stop */
	/* Delay time (step size = 8/LLC) = Recommended value for raw data type */
	somagic_write_i2c(0x4a, 0x07, 0x0d);

	/* Subaddress 0x08, Sync control */ 
	/* Automatic field detection (AUFD) = Automatic field detection */
	/* Field selection (FSEL) = 50 Hz, 625 lines (Note: Ignored due to automatic field detection) */
	/* Forced ODD/EVEN toggle FOET = ODD/EVEN signal toggles only with interlaced source */
	/* Horizontal time constant selection = Fast locking mode (recommended setting) */
	/* Horizontal PLL (HPLL) = PLL closed */
	/* Vertical noise reduction (VNOI) = Normal mode (recommended setting) */
	somagic_write_i2c(0x4a, 0x08, 0x98);

	/* Subaddress 0x09, Luminance control */ 
	/* Aperture factor (APER) = 0.25 */
	/* Update time interval for analog AGC value (UPTCV) = Horizontal update (once per line) */
	/* Vertical blanking luminance bypass (VBLB) = Active luminance processing */
	/* Chrominance trap bypass (BYPS) = Chrominance trap active; default for CVBS mode */
	work = ((luminance_prefilter & 0x01) << 6) | ((luminance_mode & 0x03) << 4) | 0x01;
	if (input_type == SVIDEO) {
		/* Chrominance trap bypass (BYPS) = Chrominance trap bypassed; default for S-video mode */
		work |= 0x80;
	}
	fprintf(stderr, "Subaddress 0x09 set to %02x\n", work); 
	somagic_write_i2c(0x4a, 0x09, work);

	/* Subaddress 0x0a, Luminance brightness control */
	/* Offset = 128 (ITU level) */
	somagic_write_i2c(0x4a, 0x0a, brightness);

	/* Subaddress 0x0b, Luminance contrast control */
	/* Gain = 1.0 */
	somagic_write_i2c(0x4a, 0x0b, contrast);

	/* Subaddress 0x0c, Chrominance saturation control */
	somagic_write_i2c(0x4a, 0x0c, saturation); 

	/* Subaddress 0x0d, Chrominance hue control */
	somagic_write_i2c(0x4a, 0x0d, hue);

	/* Subaddress 0x0e, Chrominance control */
	/* Chrominance bandwidth (CHBW0 and CHBW1) = Nominal bandwidth (800 kHz) */
	/* Fast color time constant (FCTC) = Nominal time constant */
	/* Disable chrominance comb filter (DCCF) = Chrominance comb filter on (during lines determined by VREF = 1) */
	/* Color standard selection (CSTD0 to CSTD2) = If 50Hz: PAL BGHIN. If 60Hz: NTSC M. */
	/* Clear DTO (CDTO) = Disabled */
	somagic_write_i2c(0x4a, 0x0e, 0x01);

	/* Subaddress 0x0f, Chrominance gain control */
	/* Chrominance gain value = ??? (Note: only meaningful if ACGF is off) */
	/* Automatic chrominance gain control ACGC = On */
	somagic_write_i2c(0x4a, 0x0f, 0x2a);

	/* Subaddress 0x10, Format/delay control */
	/* Output format selection (OFTS0 and OFTS1), V-flag generation in SAV/EAV-codes = V-flag in SAV/EAV is generated by VREF */
	/* Fine position of HS (HDEL0 and HDEL1) (steps in 2/LLC) = 0 */
	/* VREF pulse position and length (VRLN) = see Table 46 in SAA7113H documentation */
	/* Luminance delay compensation (steps in 2/LLC) = 0 */
	somagic_write_i2c(0x4a, 0x10, 0x40);
	/*
	if (input_type == CVBS) {
		somagic_write_i2c(0x4a, 0x10, 0x40);
	} else {
		somagic_write_i2c(0x4a, 0x10, 0x00);
	}
	*/

	/* Subaddress 0x11, Output control 1 */
	/* General purpose switch [available on pin RTS1, if control bits RTSE13 to RTSE10 (subaddress 0x12) is set to 0010] = LOW */
	/* CM99 compatibility to SAA7199 (CM99) = Default value */
	/* General purpose switch [available on pin RTS0, if control bits RTSE03 to RTSE00 (subaddress 0x12) is set to 0010] = LOW */
	/* Selection of horizontal lock indicator for RTS0 and RTS1 outputs = Standard horizontal lock indicator (low-passed) */
	/* Output enable YUV data (OEYC) = Output VPO-bus active or controlled by RTS1 */
	/* Output enable real-time (OERT) = RTS0, RTCO active, RTS1 active, if RTSE13 to RTSE10 = 0000 */
	/* YUV decoder bypassed (VIPB) = Processed data to VPO output */
	/* Color on (COLO) = Automatic color killer */
	somagic_write_i2c(0x4a, 0x11, 0x0c);

	/* Subaddress 0x12, RTS0 output control/Output control 2 */
	/* RTS1 output control = 3-state, pin RTS1 is used as DOT input */
	/* RTS0 output control = VIPB (subaddress 0x11, bit 1) = 0: reserved */ 
	somagic_write_i2c(0x4a, 0x12, 0x01);

	/* Subaddress 0x13, Output control 3 */
	if (input_type == CVBS) {
		/* Analog-to-digital converter output bits on VPO7 to VPO0 in bypass mode (VIPB = 1, used for test purposes) (ADLSB) = AD7 to AD0 (LSBs) on VPO7 to VPO0 */
		/* Selection bit for status byte functionality (OLDSB) = Default status information */
		/* Field ID polarity if selected on RTS1 or RTS0 outputs if RTSE1 and RTSE0 (subaddress 0x12) are set to 1111 = Default */
		/* Analog test select (AOSL) = AOUT connected to internal test point 1 */
		somagic_write_i2c(0x4a, 0x13, 0x80);
	} else {
		/* Analog-to-digital converter output bits on VPO7 to VPO0 in bypass mode (VIPB = 1, used for test purposes) (ADLSB) = AD8 to AD1 (MSBs) on VPO7 to VPO0 */
		/* Selection bit for status byte functionality (OLDSB) = Default status information */
		/* Field ID polarity if selected on RTS1 or RTS0 outputs if RTSE1 and RTSE0 (subaddress 0x12) are set to 1111 = Default */
		/* Analog test select (AOSL) = AOUT connected to internal test point 1 */
		somagic_write_i2c(0x4a, 0x13, 0x00);
	}

	/* Subaddress 0x15, Start of VGATE pulse (01-transition) and polarity change of FID pulse/V_GATE1_START */
	/* Note: Dependency on subaddress 0x17 value */
	/* Frame line counting = If 50Hz: 1st = 2, 2nd = 315. If 60Hz: 1st = 5, 2nd = 268. */
	somagic_write_i2c(0x4a, 0x15, 0x00);

	/* Subaddress 0x16, Stop of VGATE pulse (10-transition)/V_GATE1_STOP */
	/* Note: Dependency on subaddress 0x17 value */
	/* Frame line counting = If 50Hz: 1st = 2, 2nd = 315. If 60Hz: 1st = 5, 2nd = 268. */
	somagic_write_i2c(0x4a, 0x16, 0x00);

	/* Subaddress 0x17, VGATE MSBs/V_GATE1_MSB */
	/* VSTA8, MSB VGATE start = 0 */
	/* VSTO8, MSB VGATE stop = 0 */
	somagic_write_i2c(0x4a, 0x17, 0x00);

	/* Subaddress 0x40, AC1 */
	if (tv_standard == PAL) {
		/* Data slicer clock selection, Amplitude searching = 13.5 MHz (default) */
		/* Amplitude searching = Amplitude searching active (default) */
		/* Framing code error = One framing code error allowed */
		/* Hamming check = Hamming check for 2 bytes after framing code, dependent on data type (default) */
		/* Field size select = 50 Hz field rate */
		somagic_write_i2c(0x4a, 0x40, 0x02);
	} else {
		/* Data slicer clock selection, Amplitude searching = 13.5 MHz (default) */
		/* Amplitude searching = Amplitude searching active (default) */
		/* Framing code error = One framing code error allowed */
		/* Hamming check = Hamming check for 2 bytes after framing code, dependent on data type (default) */
		/* Field size select = 60 Hz field rate */
		somagic_write_i2c(0x4a, 0x40, 0x82);
	}

	if (input_type == CVBS) {
		/* LCR register 2 to 24 = Intercast, oversampled CVBS data */
		somagic_write_i2c(0x4a, 0x41, 0x77);
		somagic_write_i2c(0x4a, 0x42, 0x77);
		somagic_write_i2c(0x4a, 0x43, 0x77);
		somagic_write_i2c(0x4a, 0x44, 0x77);
		somagic_write_i2c(0x4a, 0x45, 0x77);
		somagic_write_i2c(0x4a, 0x46, 0x77);
		somagic_write_i2c(0x4a, 0x47, 0x77);
		somagic_write_i2c(0x4a, 0x48, 0x77);
		somagic_write_i2c(0x4a, 0x49, 0x77);
		somagic_write_i2c(0x4a, 0x4a, 0x77);
		somagic_write_i2c(0x4a, 0x4b, 0x77);
		somagic_write_i2c(0x4a, 0x4c, 0x77);
		somagic_write_i2c(0x4a, 0x4d, 0x77);
		somagic_write_i2c(0x4a, 0x4e, 0x77);
		somagic_write_i2c(0x4a, 0x4f, 0x77);
		somagic_write_i2c(0x4a, 0x50, 0x77);
		somagic_write_i2c(0x4a, 0x51, 0x77);
		somagic_write_i2c(0x4a, 0x52, 0x77);
		somagic_write_i2c(0x4a, 0x53, 0x77);
		somagic_write_i2c(0x4a, 0x54, 0x77);
		/* LCR register 2 to 24 = Active video, video component signal, active video region (default) */
		somagic_write_i2c(0x4a, 0x55, 0xff);
	}

	/* Subaddress 0x58, Framing code for programmable data types/FC */
	/* Slicer set, Programmable framing code = ??? */
	somagic_write_i2c(0x4a, 0x58, 0x00);

	/* Subaddress 0x59, Horizontal offset/HOFF */
	/* Slicer set, Horizontal offset = Recommended value */
	somagic_write_i2c(0x4a, 0x59, 0x54);

	/* Subaddress 0x5a: Vertical offset/VOFF */
	if (tv_standard == PAL) {
		/* Slicer set, Vertical offset = Value for 50 Hz 625 lines input */
		somagic_write_i2c(0x4a, 0x5a, 0x07);
	} else {
		/* Slicer set, Vertical offset = Value for 60 Hz 525 lines input */
		somagic_write_i2c(0x4a, 0x5a, 0x0a);
	}

	/* Subaddress 0x5b, Field offset, MSBs for vertical and horizontal offsets/HVOFF */
	/* Slicer set, Field offset = Invert field indicator (even/odd; default) */
	somagic_write_i2c(0x4a, 0x5b, 0x83);

	/* Subaddress 0x5e, SDID codes */
	/* Slicer set, SDID codes = SDID5 to SDID0 = 0x00 (default) */
	somagic_write_i2c(0x4a, 0x5e, 0x00);

	status = somagic_read_i2c(0x4a, 0x10);
	fprintf(stderr,"i2c_read(0x10) = %02x\n", status);

	status = somagic_read_i2c(0x4a, 0x02);
	fprintf(stderr,"i2c_stat(0x02) = %02x\n", status);

	somagic_write_reg(0x1740, 0x40);

	status = somagic_read_reg(0x3080);
	fprintf(stderr, "status is %02x\n", status);

	somagic_write_reg(0x1740, 0x00);
	usleep(250 * 1000);
	somagic_write_reg(0x1740, 0x00);

	status = somagic_read_reg(0x3080);
	fprintf(stderr, "status is %02x\n", status);

	memcpy(buf, "\x01\x05", 2);
	ret = libusb_control_transfer(devh, LIBUSB_REQUEST_TYPE_VENDOR + LIBUSB_RECIPIENT_DEVICE, 0x0000001, 0x0000001, 0x0000000, buf, 2, 1000);
	fprintf(stderr, "190 control msg returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	ret = libusb_get_descriptor(devh, 0x0000002, 0x0000000, buf, 0x0000109);
	fprintf(stderr, "191 get descriptor returned %d, bytes: ", ret);
	print_bytes(buf, ret);
	fprintf(stderr, "\n");
	ret = libusb_set_interface_alt_setting(devh, 0, 2);
	fprintf(stderr, "192 set alternate setting returned %d\n", ret);

	somagic_write_reg(0x1740, 0x00);
	usleep(30 * 1000);
	
	for (i = 0; i < NUM_ISO_TRANSFERS; i++)	{
		tfr[i] = libusb_alloc_transfer(64);
		if (tfr[i] == NULL) {
			fprintf(stderr, "Failed to allocate USB transfer #%d\n", i);
			return 1;
		}
		libusb_fill_iso_transfer(tfr[i], devh, 0x00000082, isobuf[i], 64 * 3072, 64, gotdata, NULL, 2000);
		libusb_set_iso_packet_lengths(tfr[i], 3072);
	}
	
	pending_requests = NUM_ISO_TRANSFERS;
	for (i = 0; i < NUM_ISO_TRANSFERS; i++) {
		ret = libusb_submit_transfer(tfr[i]);
		if (ret != 0) {
			fprintf(stderr, "libusb_submit_transfer failed with error %d for transfer %d\n", ret, i);
		exit(1);
		}
	}
		
	somagic_write_reg(0x1800, 0x0d);

	while (pending_requests > 0) {
		libusb_handle_events(NULL);
	}
	
	for (i = 0; i < NUM_ISO_TRANSFERS; i++) {
		libusb_free_transfer(tfr[i]);
	}

	ret = libusb_release_interface(devh, 0);
	if (ret != 0) {
		perror("Failed to release interface");
		return 1;
	}
	libusb_close(devh);
	libusb_exit(NULL);
	return 0;
}
