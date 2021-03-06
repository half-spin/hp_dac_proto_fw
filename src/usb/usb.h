#ifndef USB_H
#define USB_H
    
#include <USBFS.h>

#define USBFS_AUDIO_DEVICE  (0u)
#define AUDIO_INTERFACE     (1u)
#define AUDIO_OUT_EP        (1u)
#define AUDIO_IN_EP         (2u)
#define AUDIO_FB_EP         (3u)
#define USB_MAX_BUF_SIZE    (294u)

#define USB_STS_INACTIVE    (0x00u)
#define USB_STS_INIT        (0x01u)
#define USB_STS_ENUM        (0x02u)

#define USB_NO_STREAM_IFACE (2u)
#define USB_OUT_IFACE_INDEX (0u)
#define USB_IN_IFACE_INDEX  (1u)
#define USB_ALT_ZEROBW      (0u)
#define USB_ALT_ACTIVE_24   (2u)
#define USB_ALT_ACTIVE_16   (1u)
#define USB_ALT_INVALID     (0xFF)

// Asynchronous feedback register.
extern volatile uint8_t fb_data[3];
extern volatile uint8_t fb_updated;
extern uint32_t sample_rate_feedback;

extern uint8_t usb_status;
extern uint8_t usb_alt_setting[USB_NO_STREAM_IFACE];

// Signals a new feedback value update. Used for testing.
extern volatile uint8_t fb_update_flag;

// Set up USB and which buffer audio output gets put into.
void usb_start(uint8_t *usb_out_buf, size_t buf_size);
// Called every 128 samples when feedback ep is updated. Put this in cyapicallbacks as USBFS EP3 Entry Callback.
void usb_feedback(void);
// Call in main loop to handle usb stuff
void usb_service(void);

#endif
    