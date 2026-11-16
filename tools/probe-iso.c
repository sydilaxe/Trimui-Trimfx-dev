/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Can the Brick's bottom port, through FunctionFS, be a standard USB audio device? That needs
 * isochronous endpoints (audio on a fixed timetable), which a kernel and a USB controller may or may
 * not give FunctionFS. This sets up a test gadget offering one isochronous endpoint each way, binds it
 * to the port for a moment (as the apps' clock link does at every start; with nothing plugged into the
 * bottom port, nothing else happens), reports each step, and takes everything down again.
 *
 * Run by tools/probe-gadget.sh (as root, no app running). Built for the Brick with zig:
 *   zig cc -target aarch64-linux-gnu.2.28 -O2 -o probe-iso probe-iso.c */
#define _GNU_SOURCE
#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define G "/sys/kernel/config/usb_gadget/brickioiso"
#define FFS_DIR "/tmp/brickioiso-ffs"

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return 0;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

/* in the clock link's order (link.c's teardown, proven on the Bricks): let go of the port, unmount,
 * then unlink and remove (ep0 is closed before this) */
static void teardown(void)
{
    put(G "/UDC", "\n");
    umount(FFS_DIR);
    rmdir(FFS_DIR);
    unlink(G "/configs/c.1/ffs.brickioiso");
    rmdir(G "/configs/c.1/strings/0x409");
    rmdir(G "/configs/c.1");
    rmdir(G "/functions/ffs.brickioiso");
    rmdir(G "/strings/0x409");
    rmdir(G);
}

/* one interface, an isochronous endpoint each way (asynchronous, 48 kHz stereo 24-bit and some:
 * 294 bytes a millisecond); full speed (1 ms) and high speed (bInterval 4: 1 ms) */
typedef struct {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio out, in;
} __attribute__((packed)) Descs;

static int descriptors(int ep0)
{
    struct {
        struct usb_functionfs_descs_head_v2 head;
        __le32 fs_count, hs_count;
        Descs fs, hs;
    } __attribute__((packed)) d;
    memset(&d, 0, sizeof d);
    d.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.head.length = htole32(sizeof d);
    d.fs_count = d.hs_count = htole32(3);
    Descs *s[2] = {&d.fs, &d.hs};
    for (int k = 0; k < 2; k++) {
        s[k]->intf.bLength = USB_DT_INTERFACE_SIZE;
        s[k]->intf.bDescriptorType = USB_DT_INTERFACE;
        s[k]->intf.bNumEndpoints = 2;
        s[k]->intf.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
        s[k]->intf.iInterface = 1;
        s[k]->out.bLength = s[k]->in.bLength = USB_DT_ENDPOINT_SIZE;
        s[k]->out.bDescriptorType = s[k]->in.bDescriptorType = USB_DT_ENDPOINT;
        s[k]->out.bEndpointAddress = 1 | USB_DIR_OUT;
        s[k]->in.bEndpointAddress = 2 | USB_DIR_IN;
        s[k]->out.bmAttributes = s[k]->in.bmAttributes = USB_ENDPOINT_XFER_ISOC | USB_ENDPOINT_SYNC_ASYNC;
        s[k]->out.wMaxPacketSize = s[k]->in.wMaxPacketSize = htole16(294);
        s[k]->out.bInterval = s[k]->in.bInterval = k ? 4 : 1;
    }
    if (write(ep0, &d, sizeof d) != (ssize_t)sizeof d) return -errno;
    struct {
        struct usb_functionfs_strings_head head;
        struct {
            __le16 code;
            char str[17]; /* "brickio iso test" and its NUL */
        } __attribute__((packed)) lang;
    } __attribute__((packed)) str;
    memset(&str, 0, sizeof str);
    str.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    str.head.length = htole32(sizeof str);
    str.head.str_count = htole32(1);
    str.head.lang_count = htole32(1);
    str.lang.code = htole16(0x0409);
    memcpy(str.lang.str, "brickio iso test", 17);
    if (write(ep0, &str, sizeof str) != (ssize_t)sizeof str) return -errno;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0); /* each line out at once: a step that hangs still shows */
    char udc[300] = "";
    DIR *d = opendir("/sys/class/udc");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') snprintf(udc, sizeof udc, "%s", e->d_name);
    if (d) closedir(d);
    if (!*udc) { printf("iso: no USB device controller\n"); return 0; }
    DIR *g = opendir("/sys/kernel/config/usb_gadget");
    while (g && (e = readdir(g)))
        if (e->d_name[0] != '.') {
            char p[300], v[64] = "";
            snprintf(p, sizeof p, "/sys/kernel/config/usb_gadget/%s/UDC", e->d_name);
            int fd = open(p, O_RDONLY);
            if (fd >= 0) { ssize_t r = read(fd, v, sizeof v - 1); close(fd); if (r > 1) { printf("iso: the port is in use (gadget %s): run with no app open\n", e->d_name); closedir(g); return 0; } }
        }
    if (g) closedir(g);
    teardown();
    int ok = mkdir(G, 0755) == 0 && put(G "/idVendor", "0x1d6b") && put(G "/idProduct", "0x0105") && mkdir(G "/strings/0x409", 0755) == 0 &&
             put(G "/strings/0x409/product", "brickio iso test") && mkdir(G "/configs/c.1", 0755) == 0 &&
             mkdir(G "/functions/ffs.brickioiso", 0755) == 0 && mkdir(FFS_DIR, 0755) == 0 &&
             mount("brickioiso", FFS_DIR, "functionfs", 0, NULL) == 0;
    if (!ok) { printf("iso: couldn't set up the test gadget (%s)\n", strerror(errno)); teardown(); return 0; }
    printf("iso: test gadget set up, FunctionFS mounted\n");
    fflush(stdout);
    int ep0 = open(FFS_DIR "/ep0", O_RDWR);
    int r = ep0 < 0 ? -errno : descriptors(ep0);
    if (r < 0) {
        printf("iso: FunctionFS refused isochronous endpoints (%s): the receiving Brick can't be a standard USB audio device\n", strerror(-r));
        if (ep0 >= 0) close(ep0);
        teardown();
        return 0;
    }
    printf("iso: FunctionFS took the isochronous endpoints\n");
    if (symlink(G "/functions/ffs.brickioiso", G "/configs/c.1/ffs.brickioiso") != 0) printf("iso: couldn't link the function (%s)\n", strerror(errno));
    char line[310];
    snprintf(line, sizeof line, "%s\n", udc);
    if (put(G "/UDC", line)) {
        printf("iso: AVAILABLE: the USB controller (%s) bound them: a receiving Brick can be a standard USB audio device\n", udc);
        usleep(300000);
    } else
        printf("iso: the USB controller (%s) couldn't bind them (%s): no standard USB audio device here, brickio's own channel instead\n", udc,
               strerror(errno));
    printf("iso: taking it down...\n");
    fflush(stdout);
    put(G "/UDC", "\n");
    close(ep0);
    teardown();
    printf("iso: test gadget removed\n");
    return 0;
}
