/* Local USB screen/debug transport. All fields are little endian. */
#ifndef T290_USB_PROTOCOL_H
#define T290_USB_PROTOCOL_H
#if defined(KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif
#define T290_USB_VID 0x1209
#define T290_USB_PID 0x0001
#define T290_USB_MAGIC "T290USB1"
#define T290_USB_FRAME 1
#define T290_USB_LOG 2
#define T290_USB_INFO 3
#define T290_USB_ERROR 4
#define T290_USB_RBG3 0x52424733u
#define T290_USB_MAX_FRAME (800u * 1280u * 3u)
struct t290_usb_header {
    char magic[8];
    uint32_t version, type, sequence, width, height, stride, format, shift;
    uint64_t bytes, nanoseconds, reserved;
};
_Static_assert(sizeof(struct t290_usb_header) == 64, "USB wire header");
#endif
