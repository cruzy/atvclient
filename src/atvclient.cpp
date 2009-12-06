/*
 * atvclient: AppleTV Remote XBMC client
 *
 * Copyright (C) 2009 Christoph Cantillon <christoph.cantillon@roots.be> 
 * Copyright (C) 2008 Peter Korsgaard <jacmet@sunsite.dk>
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <usb.h>
#include <sys/time.h>

#include "xbmcclient.h"

#define VENDOR_APPLE		0x05ac
#define PRODUCT_IR		0x8241

#define LEDMODE_OFF		0
#define LEDMODE_AMBER		1
#define LEDMODE_AMBER_BLINK	2
#define LEDMODE_WHITE		3
#define LEDMODE_WHITE_BLINK	4
#define LEDMODE_BOTH		5
#define LEDMODE_MAX		5

#define BUTTON_TIMEOUT 150
#define HOLD_TIMEOUT   500
#define REPEAT_TIMEOUT 500

#define EVENT_UP 1
#define EVENT_DOWN 2
#define EVENT_LEFT 3
#define EVENT_RIGHT 4
#define EVENT_PLAY 5
#define EVENT_MENU 6
#define EVENT_HOLD_PLAY 7
#define EVENT_HOLD_MENU 8

/* from libusb usbi.h */
struct usb_dev_handle {
	int fd;

	struct usb_bus *bus;
	struct usb_device *device;

	int config;
	int interface;
	int altsetting;

	/* Added by RMT so implementations can store other per-open-device data */
	void *impl_info;
};

struct ir_command {
  unsigned char flags;
  unsigned char unused;
  unsigned char event;
  unsigned char address;
  unsigned char eventId;
};

static CAddress my_addr;
static int sockfd;

static CPacketBUTTON* button_map[9];

static CPacketNOTIFICATION remote_paired("Remote paired", "Your remote is now paired.", NULL, NULL);
static CPacketNOTIFICATION remote_unpaired("Remote unpaired", "Your remote is now unpaired.", NULL, NULL); 

/* figure out kernel name corresponding to usb device */
static int usb_make_kernel_name(usb_dev_handle *dev, int interface,
				char *name, int namelen)
{
	DIR *dir;
	struct dirent *ent;
	char busstr[10], devstr[10];
	int buslen, devlen;

	/* kernel names are in the form of:
	   <busnum>-<devpath>:<config>.<interface>
	   We have everything besides devpath, but there doesn't seem to be
	   an easy of going from devnum to devpath, so we scan sysfs */

	buslen = sprintf(busstr, "%d-", atoi(dev->bus->dirname));
	devlen = sprintf(devstr, "%d\n", dev->device->devnum);

	/* scan /sys/bus/usb/devices/<busnum>-* and compare devnum */
	if (chdir("/sys/bus/usb/devices"))
		return -1;

	dir = opendir(".");
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		char buf[PATH_MAX];
		int fd;

		/* only check devices on busnum bus */
		if (strncmp(busstr, ent->d_name, buslen))
			continue;

		sprintf(buf, "%s/devnum", ent->d_name);
		fd = open(buf, O_RDONLY);
		if (fd == -1)
			continue;

		if ((read(fd, buf, sizeof(buf)) == devlen)
		    && !strncmp(buf, devstr, devlen)) {

			close(fd);

			if (snprintf(name, namelen, "%s:%d.%d", ent->d_name,
				     1, interface) >= namelen)
				goto out;

			/* closedir could invalidate ent, so do it after the
			   snprintf */
			closedir(dir);
			return 0;
		}

		close(fd);
	}

 out:
	closedir(dir);
	return -1;
}

/* (re)attach usb device to kernel driver (need hotplug support in kernel) */
static int usb_attach_kernel_driver_np(usb_dev_handle *dev, int interface,
				       const char *driver)
{
	char name[PATH_MAX], buf[PATH_MAX];

	if (!dev || !driver || interface < 0)
		return -1;

	if (!usb_make_kernel_name(dev, interface, name, sizeof(name))) {
		int fd, ret, len;

		/* (re)bind driver to device */
		sprintf(buf, "/sys/bus/usb/drivers/%s/bind", driver);
		len = strlen(name);

		fd = open(buf, O_WRONLY);
		if (fd == -1)
			return -1;

		ret = write(fd, name, len);
		close(fd);

		if (ret != len)
			return -1;
		else
			return 0;
	}

	return -1;
}

static usb_dev_handle *find_ir(void)
{
	struct usb_bus *bus;
	struct usb_device *dev;

	for (bus = usb_busses; bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next)
			if (dev->descriptor.idVendor == VENDOR_APPLE
			    && dev->descriptor.idProduct == PRODUCT_IR)
				return usb_open(dev);
	}

	return NULL;
}

static usb_dev_handle *get_ir(void)
{
	static usb_dev_handle *ir = NULL;

	if (!ir) {
		usb_init();
		usb_find_busses();
		usb_find_devices();

		ir = find_ir();
		if (!ir) {
			fprintf(stderr, "IR receiver not found, quitting\n");
			exit(1);
		}

		/* interface is normally handled by hiddev */
		usb_detach_kernel_driver_np(ir, 0);
		if (usb_claim_interface(ir, 0)) {
			fprintf(stderr, "error claiming interface, are you root?\n");
			exit(2);
		}
		//usb_reset(ir);
		//usb_set_configuration(ir, 0);
	}

	return ir;
}

static void reattach(void)
{
	usb_dev_handle *ir;

	ir = get_ir();
	if (ir) {
		usb_release_interface(ir, 0);
		/* attach fails if we still have the file
		   descriptor open */
		usb_close(ir);
		usb_attach_kernel_driver_np(ir, 0, "usbhid");
	}
}

static int set_report(unsigned char* data, int len)
{
	unsigned char *type = data;
	int val;

	val = 0x300 | *type;

	return (usb_control_msg(get_ir(), USB_ENDPOINT_OUT | USB_TYPE_CLASS
				| USB_RECIP_INTERFACE, 9, val, 0,
				(char*) data, len, 1000) != len);
}

static void set_fan(int full)
{
	unsigned char buf[2];

	buf[0] = 0xf; buf[1] = full ? 1 : 0;

	set_report(buf, sizeof(buf));
}

static void set_led(int mode)
{
	unsigned char buf[5];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xd; buf[1] = mode;

	switch (mode) {
	case LEDMODE_OFF:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		break;

	case LEDMODE_AMBER:
		set_report(buf, sizeof(buf));
		buf[1] = 3; buf[2] = 1;
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 0;
		set_report(buf, 3);
		break;

	case LEDMODE_AMBER_BLINK:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		buf[1] = 3; buf[2] = 2;
		set_report(buf, 3);
		break;

	case LEDMODE_WHITE:
		set_report(buf, sizeof(buf));
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 1;
		set_report(buf, 3);
		break;

	case LEDMODE_WHITE_BLINK:
		set_report(buf, sizeof(buf));
		buf[1] = 3;
		set_report(buf, 3);
		buf[1] = 4;
		set_report(buf, 3);
		buf[1] = 4; buf[2] = 2;
		set_report(buf, 3);
		break;

	case LEDMODE_BOTH:
		buf[1] = 7;
		set_report(buf, sizeof(buf));
		buf[1] = 6; buf[2] = 1;
		set_report(buf, 3);
		break;
	}
}

static void set_led_brightness(int high)
{
	unsigned char buf[5];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0xd;

	if (high) {
		buf[1] = 6;
		set_report(buf, sizeof(buf));
		buf[1] = 5; buf[2] = 1;
		set_report(buf, 3);
	} else {
		buf[1] = 5;
		set_report(buf, sizeof(buf));
		set_report(buf, 3);
	}
}

void dumphex(unsigned char* buf, int size) {
  int i;
  for(i=0; i < size; i++) {
    printf("%02x ", buf[i]);
  }
  printf("\n");

}

unsigned long millis() {
  static struct timeval time;
  gettimeofday(&time, NULL);
  return (time.tv_sec*1000+time.tv_usec/1000);
}

void send_button(int button) {
  switch(button) {
    case EVENT_UP: printf("Up\n"); break;
    case EVENT_DOWN: printf("Down\n"); break;
    case EVENT_LEFT: printf("Left\n"); break;
    case EVENT_RIGHT: printf("Right\n"); break;
    case EVENT_MENU: printf("Menu\n"); break;
    case EVENT_HOLD_MENU: printf("Menu hold\n"); break;
    case EVENT_PLAY: printf("Play/pause\n"); break;
    case EVENT_HOLD_PLAY: printf("Play/pause hold\n"); break;
  }
  
  printf("%d %x\n", button, button_map[button]);
  
  /* CPacketBUTTON btn(button, "JS0:AppleRemote", BTN_DOWN);
  btn.Send(sockfd, my_addr); */
  button_map[button] -> Send(sockfd, my_addr);
}

void handle_button(struct ir_command command) {
  static unsigned char previousButton;
  static unsigned char holdButtonSent;
  static long buttonStart;

  if(previousButton != command.eventId && previousButton != 0 && !holdButtonSent) {
    switch(previousButton) {
      case 0x03: case 0x02: send_button(EVENT_MENU); break;
      case 0x05: case 0x04: send_button(EVENT_PLAY); break;
    }
  }
  
  if(previousButton != command.eventId) {
    buttonStart = millis();
  } else {
    if(command.flags == 0x26 && millis() - buttonStart < REPEAT_TIMEOUT) return;
  }

  switch(command.eventId) {
    case 0x0a:
    case 0x0b:
      send_button(EVENT_UP); break;
    case 0x0c:
    case 0x0d:
      send_button(EVENT_DOWN); break;
    case 0x09:
    case 0x08:
      send_button(EVENT_LEFT); break;
    case 0x06:
    case 0x07:
      send_button(EVENT_RIGHT); break;
    case 0x03: case 0x02: case 0x05: case 0x04:
      // menu and pause need special treatment
      if(previousButton != command.eventId) {
        holdButtonSent = 0;
      } else {
        if(millis() - buttonStart > HOLD_TIMEOUT && !holdButtonSent) {
          switch(command.eventId) {
            case 0x03: case 0x02: send_button(EVENT_HOLD_MENU); break;
            case 0x05: case 0x04: send_button(EVENT_HOLD_PLAY); break;
          }
          holdButtonSent = 1;
        }
      }
      break;
    case 0x00: break; //timeout
    default:
      printf("unknown\n");
  }
  previousButton = command.eventId;
}

void handle_special(struct ir_command command) {
  static unsigned char previousEventId;
  static long previousEventTime;
  
  if(command.eventId != previousEventId || millis() - previousEventTime > REPEAT_TIMEOUT) {
    switch(command.eventId) {
      case 0x02: // pair!
        printf("Pairing ID: %d\n", command.address);
        remote_paired.Send(sockfd, my_addr);
        break;
      case 0x04: // unpair!
        printf("Unpairing ID: %d\n", command.address);
        remote_unpaired.Send(sockfd, my_addr);
        break;  
      case 0x03: // update!
        printf("Updating Remote ID: %d\n", command.address);
        break;
    }
    previousEventId = command.eventId;
  }      
  previousEventTime = millis();
  
}

int main(int argc, char **argv) {

  struct ir_command command;
  struct ir_command timeoutCommand;
  
  memset(&timeoutCommand, 0, sizeof(timeoutCommand));
  
  printf("Creating socket...\n");
  
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
  {
    printf("Error creating socket\n");
    return -1;
  }

  my_addr.Bind(sockfd);
 
  printf("Preparing button map...\n");
 
  button_map[EVENT_UP]          = new CPacketBUTTON(EVENT_UP,         "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_DOWN]        = new CPacketBUTTON(EVENT_DOWN,       "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_LEFT]        = new CPacketBUTTON(EVENT_LEFT,       "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_RIGHT]       = new CPacketBUTTON(EVENT_RIGHT,      "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_PLAY]        = new CPacketBUTTON(EVENT_PLAY,       "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_MENU]        = new CPacketBUTTON(EVENT_MENU,       "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_HOLD_PLAY]   = new CPacketBUTTON(EVENT_HOLD_PLAY,  "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  button_map[EVENT_HOLD_MENU]   = new CPacketBUTTON(EVENT_HOLD_MENU,  "JS0:AppleRemote", BTN_DOWN | BTN_NO_REPEAT | BTN_QUEUE);
  
  printf("Ready!\n");
  
  set_led(LEDMODE_WHITE);
  //usb_clear_halt(get_ir(), 0x82);
  
  char isRepeat;           
                          
  while(1){
    int result = usb_interrupt_read(get_ir(), 0x82, (char*) &command, sizeof(command), BUTTON_TIMEOUT);  
    
    if(result > 0) {
      // we have an IR code!
      set_led(LEDMODE_OFF);
      unsigned long start = millis();
      //printf("%10d: ", millis());
      dumphex((unsigned char*) &command, result);
       
      switch(command.event) {
        case 0xee: handle_button(command); break;
        case 0xe0: handle_special(command); break;
        default: printf("Unknown event %x\n", command.event);
      }
      
    } else if(result == -110) {
      // timeout, reset led                        
      set_led(LEDMODE_WHITE);
      handle_button(timeoutCommand);
    } else {
      // something else
      printf("Got nuffing: %d\n", result);
    }
  }
  reattach();
}