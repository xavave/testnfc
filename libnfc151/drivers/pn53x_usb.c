/*-
 * Public platform independent Near Field Communication (NFC) library
 * 
 * Copyright (C) 2009, Roel Verdult
 * Copyright (C) 2010, Romain Tartière, Romuald Conty
 * Copyright (C) 2011, Romain Tartière, Romuald Conty
 * 
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

/**
 * @file pn53x_usb.c
 * @brief Driver for PN53x using USB
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif // HAVE_CONFIG_H

/*
Thanks to d18c7db and Okko for example code
*/

#include "../../win32/sys/select.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _WIN32
  // Under POSIX system, we use libusb (>= 0.1.12)
  #include <usb.h>
  #define USB_TIMEDOUT ETIMEDOUT
  #define _usb_strerror( X ) strerror(-X)
#else
  // Under Windows we use libusb-win32 (>= 1.2.5)
  #include "lusb0_usb.h"
  #define USB_TIMEDOUT 116
  #define _usb_strerror( X ) usb_strerror()
#endif

#include <string.h>

#include "../nfc/nfc.h"
#include "../nfc/nfc-types.h"

#include "../nfc-internal.h"
#include "../chips/pn53x.h"
#include "../chips/pn53x-internal.h"
#include "../drivers/pn53x_usb.h"
#include <sys\types.h>
#include <winsock.h>
#define PN53X_USB_DRIVER_NAME "PN53x USB"
#define LOG_CATEGORY "libnfc.driver.pn53x_usb"

#define USB_INFINITE_TIMEOUT   0

#define DRIVER_DATA(pnd) ((struct pn53x_usb_data*)(pnd->driver_data))

/* This modified from some GNU example _not_ to overwrite y */
int timeval_subtract(struct timeval *result,
		     const struct timeval *x,
		     const struct timeval *y)
{
  struct timeval tmp;

  tmp.tv_sec = y->tv_sec;
  tmp.tv_usec = y->tv_usec;

  /* Perform the carry for the later subtraction */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    tmp.tv_usec -= 1000000 * nsec;
    tmp.tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    tmp.tv_usec += 1000000 * nsec;
    tmp.tv_sec -= nsec;
  }
     
  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - tmp.tv_sec;
  result->tv_usec = x->tv_usec - tmp.tv_usec;
     
  /* Return 1 if result is negative. */
  return x->tv_sec < tmp.tv_sec;
}

typedef enum {
  UNKNOWN,
  NXP_PN531,
  SONY_PN531,
  NXP_PN533,
  ASK_LOGO,
  SCM_SCL3711,
  SONY_RCS360
} pn53x_usb_model;

struct pn53x_usb_data {
  usb_dev_handle *pudh;
  pn53x_usb_model model;
  uint32_t uiEndPointIn;
  uint32_t uiEndPointOut;
  uint32_t uiMaxPacketSize;
  volatile bool abort_flag;
};

const struct pn53x_io pn53x_usb_io;
bool pn53x_usb_get_usb_device_name (struct usb_device *dev, usb_dev_handle *udev, char *buffer, size_t len);
bool pn53x_usb_init (nfc_device_t *pnd);

int
pn53x_usb_bulk_read (struct pn53x_usb_data *data, byte_t abtRx[], const size_t szRx, struct timeval *timeout)
{
  int timeout_ms = USB_INFINITE_TIMEOUT;
  if (timeout) {
    timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
    if (timeout_ms == USB_INFINITE_TIMEOUT) {
      // timeout < 1 ms
      timeout_ms++;
    }
  }

  int res = usb_bulk_read (data->pudh, data->uiEndPointIn, (char *) abtRx, szRx, timeout_ms);
  if (res > 0) {
    LOG_HEX ("RX", abtRx, res);
  } else if (res < 0) {
    if (res != -USB_TIMEDOUT)
      log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to read from USB (%s)", _usb_strerror (res));
  }
  return res;
}

int
pn53x_usb_bulk_write (struct pn53x_usb_data *data, byte_t abtTx[], const size_t szTx, struct timeval *timeout)
{
  LOG_HEX ("TX", abtTx, szTx);
  int timeout_ms = USB_INFINITE_TIMEOUT;
  if (timeout)
      timeout_ms = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;

  int res = usb_bulk_write (data->pudh, data->uiEndPointOut, (char *) abtTx, szTx, timeout_ms);
  if (res > 0) {
    // HACK This little hack is a well know problem of USB, see http://www.libusb.org/ticket/6 for more details
    if ((res % data->uiMaxPacketSize) == 0) {
      usb_bulk_write (data->pudh, data->uiEndPointOut, "\0", 0, timeout_ms);
    }
  } else {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to write to USB (%s)", _usb_strerror (res));
  }
  return res;
}

struct pn53x_usb_supported_device {
  uint16_t vendor_id;
  uint16_t product_id;
  pn53x_usb_model model;
  const char *name;
};

const struct pn53x_usb_supported_device pn53x_usb_supported_devices[] = {
  { 0x04CC, 0x0531, NXP_PN531,   "Philips / PN531" },
  { 0x04CC, 0x2533, NXP_PN533,   "NXP / PN533" },
  { 0x04E6, 0x5591, SCM_SCL3711, "SCM Micro / SCL3711-NFC&RW" },
  { 0x054c, 0x0193, SONY_PN531,  "Sony / PN531" },
  { 0x1FD3, 0x0608, ASK_LOGO,    "ASK / LoGO" },
  { 0x054C, 0x02E1, SONY_RCS360, "Sony / FeliCa S360 [PaSoRi]" }
};

pn53x_usb_model
pn53x_usb_get_device_model (uint16_t vendor_id, uint16_t product_id)
{
  for (size_t n = 0; n < sizeof (pn53x_usb_supported_devices) / sizeof (struct pn53x_usb_supported_device); n++) {
    if ((vendor_id == pn53x_usb_supported_devices[n].vendor_id) &&
       (product_id == pn53x_usb_supported_devices[n].product_id))
      return pn53x_usb_supported_devices[n].model;
  }

  return UNKNOWN;
}

int  pn53x_usb_ack (nfc_device_t * pnd);

// Find transfer endpoints for bulk transfers
void
pn53x_usb_get_end_points (struct usb_device *dev, struct pn53x_usb_data *data)
{
  uint32_t uiIndex;
  uint32_t uiEndPoint;
  struct usb_interface_descriptor *puid = dev->config->interface->altsetting;

  // 3 Endpoints maximum: Interrupt In, Bulk In, Bulk Out
  for (uiIndex = 0; uiIndex < puid->bNumEndpoints; uiIndex++) {
    // Only accept bulk transfer endpoints (ignore interrupt endpoints)
    if (puid->endpoint[uiIndex].bmAttributes != USB_ENDPOINT_TYPE_BULK)
      continue;

    // Copy the endpoint to a local var, makes it more readable code
    uiEndPoint = puid->endpoint[uiIndex].bEndpointAddress;

    // Test if we dealing with a bulk IN endpoint
    if ((uiEndPoint & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_IN) {
      data->uiEndPointIn = uiEndPoint;
      data->uiMaxPacketSize = puid->endpoint[uiIndex].wMaxPacketSize;
    }
    // Test if we dealing with a bulk OUT endpoint
    if ((uiEndPoint & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT) {
      data->uiEndPointOut = uiEndPoint;
      data->uiMaxPacketSize = puid->endpoint[uiIndex].wMaxPacketSize;
    }
  }
}

bool
pn53x_usb_probe (nfc_device_desc_t pnddDevices[], size_t szDevices, size_t * pszDeviceFound)
{
  usb_init ();

  int res;
  // usb_find_busses will find all of the busses on the system. Returns the
  // number of changes since previous call to this function (total of new
  // busses and busses removed).
  if ((res = usb_find_busses () < 0)) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to find USB busses (%s)", _usb_strerror (res));
    return false;
  }
  // usb_find_devices will find all of the devices on each bus. This should be
  // called after usb_find_busses. Returns the number of changes since the
  // previous call to this function (total of new device and devices removed).
  if ((res = usb_find_devices () < 0)) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to find USB devices (%s)", _usb_strerror (res));
    return false;
  }

  *pszDeviceFound = 0;

  uint32_t uiBusIndex = 0;
  struct usb_bus *bus;
  for (bus = usb_get_busses (); bus; bus = bus->next) {
    struct usb_device *dev;

    for (dev = bus->devices; dev; dev = dev->next, uiBusIndex++) {
      for (size_t n = 0; n < sizeof (pn53x_usb_supported_devices) / sizeof (struct pn53x_usb_supported_device); n++) {
        if ((pn53x_usb_supported_devices[n].vendor_id == dev->descriptor.idVendor) &&
            (pn53x_usb_supported_devices[n].product_id == dev->descriptor.idProduct)) {
          // Make sure there are 2 endpoints available
          // with libusb-win32 we got some null pointers so be robust before looking at endpoints:
          if (dev->config == NULL || dev->config->interface == NULL || dev->config->interface->altsetting == NULL) {
            // Nope, we maybe want the next one, let's try to find another
            continue;
          }
          if (dev->config->interface->altsetting->bNumEndpoints < 2) {
            // Nope, we maybe want the next one, let's try to find another
            continue;
          }

          usb_dev_handle *udev = usb_open (dev);

          // Set configuration
          int res = usb_set_configuration (udev, 1);
          if (res < 0) {
            log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to set USB configuration (%s)", _usb_strerror (res));
            usb_close (udev);
            // we failed to use the device
            continue;
          }

          pn53x_usb_get_usb_device_name (dev, udev, pnddDevices[*pszDeviceFound].acDevice, sizeof (pnddDevices[*pszDeviceFound].acDevice));
          usb_close (udev);
          pnddDevices[*pszDeviceFound].pcDriver = PN53X_USB_DRIVER_NAME;
          pnddDevices[*pszDeviceFound].uiBusIndex = uiBusIndex;
          (*pszDeviceFound)++;
          // Test if we reach the maximum "wanted" devices
          if ((*pszDeviceFound) == szDevices) {
            return true;
          }
        }
      }
    }
  }

  return true;
}

bool
pn53x_usb_get_usb_device_name (struct usb_device *dev, usb_dev_handle *udev, char *buffer, size_t len)
{
  *buffer = '\0';

  if (dev->descriptor.iManufacturer || dev->descriptor.iProduct) {
    if (udev) {
      usb_get_string_simple (udev, dev->descriptor.iManufacturer, buffer, len);
      if (strlen (buffer) > 0)
        strcpy (buffer + strlen (buffer), " / ");
      usb_get_string_simple (udev, dev->descriptor.iProduct, buffer + strlen (buffer), len - strlen (buffer));
    }
  }

  if (!*buffer) {
    for (size_t n = 0; n < sizeof (pn53x_usb_supported_devices) / sizeof (struct pn53x_usb_supported_device); n++) {
      if ((pn53x_usb_supported_devices[n].vendor_id == dev->descriptor.idVendor) &&
          (pn53x_usb_supported_devices[n].product_id == dev->descriptor.idProduct)) {
        strncpy (buffer, pn53x_usb_supported_devices[n].name, len);
        return true;
      }
    }
  }

  return false;
}

nfc_device_t *
pn53x_usb_connect (const nfc_device_desc_t *pndd)
{
  nfc_device_t *pnd = NULL;
  struct pn53x_usb_data data = {
    .pudh = NULL,
    .uiEndPointIn = 0,
    .uiEndPointOut = 0,
  };
  struct usb_bus *bus;
  struct usb_device *dev;
  uint32_t uiBusIndex;

  usb_init ();

  uiBusIndex = pndd->uiBusIndex;

  for (bus = usb_get_busses (); bus; bus = bus->next) {
    for (dev = bus->devices; dev; dev = dev->next, uiBusIndex--) {
      log_put (LOG_CATEGORY, NFC_PRIORITY_TRACE, "Checking device %04x:%04x", dev->descriptor.idVendor, dev->descriptor.idProduct);
      if (uiBusIndex == 0) {
        // Open the USB device
        data.pudh = usb_open (dev);
        // Retrieve end points
        pn53x_usb_get_end_points (dev, &data);
        // Set configuration
        int res = usb_set_configuration (data.pudh, 1);
        if (res < 0) {
          log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to set USB configuration (%s)", _usb_strerror (res));
          if (EPERM == -res) {
            log_put (LOG_CATEGORY, NFC_PRIORITY_WARN, "Please double check USB permissions for device %04x:%04x", dev->descriptor.idVendor, dev->descriptor.idProduct);
          }
          usb_close (data.pudh);
          // we failed to use the specified device
          return NULL;
        }

        res = usb_claim_interface (data.pudh, 0);
        if (res < 0) {
          log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to claim USB interface (%s)", _usb_strerror (res));
          usb_close (data.pudh);
          // we failed to use the specified device
          return NULL;
        }
        data.model = pn53x_usb_get_device_model (dev->descriptor.idVendor, dev->descriptor.idProduct);
        // Allocate memory for the device info and specification, fill it and return the info
        pnd = nfc_device_new ();
        pn53x_usb_get_usb_device_name (dev, data.pudh, pnd->acName, sizeof (pnd->acName));

        pnd->driver_data = malloc(sizeof(struct pn53x_usb_data));
        *DRIVER_DATA (pnd) = data;

        // Alloc and init chip's data
        pn53x_data_new (pnd, &pn53x_usb_io);

        switch (DRIVER_DATA (pnd)->model) {
          // empirical tuning
          case ASK_LOGO:
            CHIP_DATA (pnd)->timer_correction = 50;
            break;
          case SCM_SCL3711:
          case NXP_PN533:
            CHIP_DATA (pnd)->timer_correction = 46;
            break;
          case NXP_PN531:
            CHIP_DATA (pnd)->timer_correction = 50;
            break;
          case SONY_PN531:
            CHIP_DATA (pnd)->timer_correction = 54;
            break;
          default:
            break;
        }
        pnd->driver = &pn53x_usb_driver;

        // HACK1: Send first an ACK as Abort command, to reset chip before talking to it:
        pn53x_usb_ack (pnd);

        // HACK2: Then send a GetFirmware command to resync USB toggle bit between host & device
        // in case host used set_configuration and expects the device to have reset its toggle bit, which PN53x doesn't do
        if (!pn53x_usb_init (pnd)) {
          usb_close (data.pudh);
          goto error;
        }
        DRIVER_DATA (pnd)->abort_flag = false;
        return pnd;
      }
    }
  }
  // We ran out of devices before the index required
  return NULL;

error:
  // Free allocated structure on error.
  nfc_device_free (pnd);
  return NULL;
}

void
pn53x_usb_disconnect (nfc_device_t * pnd)
{
  pn53x_usb_ack (pnd);

  pn53x_idle (pnd);

  if (DRIVER_DATA (pnd)->model == ASK_LOGO) {
    /* Set P30, P31, P32, P33, P35 to logic 1 and P34 to 0 logic */
    /* ie. Switch all LEDs off and turn off progressive field */
    pn53x_write_register (pnd, PN53X_SFR_P3, 0xFF, _BV (P30) | _BV (P31) | _BV (P32) | _BV (P33) | _BV (P35));
  }

  int res;
  if ((res = usb_release_interface (DRIVER_DATA (pnd)->pudh, 0)) < 0) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to release USB interface (%s)", _usb_strerror (res));
  }

  if ((res = usb_close (DRIVER_DATA (pnd)->pudh)) < 0) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to close USB connection (%s)", _usb_strerror (res));
  }
  pn53x_data_free (pnd);
  nfc_device_free (pnd);
}

#define PN53X_USB_BUFFER_LEN (PN53x_EXTENDED_FRAME__DATA_MAX_LEN + PN53x_EXTENDED_FRAME__OVERHEAD)

bool
pn53x_usb_send (nfc_device_t * pnd, const byte_t * pbtData, const size_t szData, struct timeval *timeout)
{
  byte_t  abtFrame[PN53X_USB_BUFFER_LEN] = { 0x00, 0x00, 0xff };  // Every packet must start with "00 00 ff"
  size_t szFrame = 0;

  pn53x_build_frame (abtFrame, &szFrame, pbtData, szData);

  int res = pn53x_usb_bulk_write (DRIVER_DATA (pnd), abtFrame, szFrame, timeout);

  if (res < 0) {
    pnd->iLastError = ECOMIO;
    return false;
  }

  byte_t abtRxBuf[PN53X_USB_BUFFER_LEN];
  res = pn53x_usb_bulk_read (DRIVER_DATA (pnd), abtRxBuf, sizeof (abtRxBuf), timeout);
  if (res < 0) {
    pnd->iLastError = ECOMIO;
    // try to interrupt current device state
    pn53x_usb_ack(pnd);
    return false;
  }

  if (pn53x_check_ack_frame (pnd, abtRxBuf, res)) {
    // The PN53x is running the sent command
  } else {
    // For some reasons (eg. send another command while a previous one is
    // running), the PN533 sometimes directly replies the response packet
    // instead of ACK frame, so we send a NACK frame to force PN533 to resend
    // response packet. With this hack, the nextly executed function (ie.
    // pn53x_usb_receive()) will be able to retreive the correct response
    // packet.
    // FIXME Sony reader is also affected by this bug but NACK is not supported
    int res = pn53x_usb_bulk_write (DRIVER_DATA (pnd), (byte_t *)pn53x_nack_frame, sizeof(pn53x_nack_frame), timeout);
    if (res < 0) {
      pnd->iLastError = ECOMIO;
      // try to interrupt current device state
      pn53x_usb_ack(pnd);
      return false;
    }
  }

  return true;
}

int
pn53x_usb_receive (nfc_device_t * pnd, byte_t * pbtData, const size_t szDataLen, struct timeval *timeout)
{
  size_t len;
  off_t offset = 0;

  byte_t  abtRxBuf[PN53X_USB_BUFFER_LEN];
  int res;

  /*
   * If no timeout is specified but the command is blocking, force a 250ms
   * timeout to allow breaking the loop if the user wants to stop it.
   */
  const struct timeval fixed_timeout = {
      .tv_sec = 0,
      .tv_usec = 250000,
  };

  struct timeval remaining_time, usb_timeout;
  if (timeout) {
    remaining_time = *timeout;
  }
read:
  if (timeout) {
    // A user-provided timeout is set, we have to cut it in multiple chunk to be able to keep an nfc_abort_command() mecanism
    struct timeval tmp;
    if (1 == timeval_subtract (&tmp, &remaining_time, &fixed_timeout)) {
      // The subtraction result is negative
      usb_timeout = remaining_time;
      remaining_time.tv_sec = 0;
      remaining_time.tv_usec = 0;
    } else {
      usb_timeout = fixed_timeout;
      remaining_time = tmp;
    }
  } else {
    // No user-provided timeout, we will wait infinitely but we need nfc_abort_command() mecanism.
    usb_timeout = fixed_timeout;
  }
  if ((usb_timeout.tv_sec == 0) && (usb_timeout.tv_usec == 0)) {
    pnd->iLastError = ECOMTIMEOUT;
    return -1;
  }
  res = pn53x_usb_bulk_read (DRIVER_DATA (pnd), abtRxBuf, sizeof (abtRxBuf), &usb_timeout);

  if (res == -USB_TIMEDOUT) {
    if (DRIVER_DATA (pnd)->abort_flag) {
      DRIVER_DATA (pnd)->abort_flag = false;
      pn53x_usb_ack (pnd);
      pnd->iLastError = EOPABORT;
      return -1;
    } else {
      goto read;
    }
  }

  if (res < 0) {
    pnd->iLastError = ECOMIO;
    // try to interrupt current device state
    pn53x_usb_ack(pnd);
    return -1;
  }

  const byte_t pn53x_preamble[3] = { 0x00, 0x00, 0xff };
  if (0 != (memcmp (abtRxBuf, pn53x_preamble, 3))) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Frame preamble+start code mismatch");
    pnd->iLastError = ECOMIO;
    return -1;
  }
  offset += 3;

  if ((0x01 == abtRxBuf[offset]) && (0xff == abtRxBuf[offset + 1])) {
    // Error frame
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Application level error detected");
    pnd->iLastError = EFRAISERRFRAME;
    return -1;
  } else if ((0xff == abtRxBuf[offset]) && (0xff == abtRxBuf[offset + 1])) {
    // Extended frame
    offset += 2;

    // (abtRxBuf[offset] << 8) + abtRxBuf[offset + 1] (LEN) include TFI + (CC+1)
    len = (abtRxBuf[offset] << 8) + abtRxBuf[offset + 1] - 2;
    if (((abtRxBuf[offset] + abtRxBuf[offset + 1] + abtRxBuf[offset + 2]) % 256) != 0) {
      // TODO: Retry
      log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Length checksum mismatch");
      pnd->iLastError = ECOMIO;
      return -1;
    }
    offset += 3;
  } else {
    // Normal frame
    if (256 != (abtRxBuf[offset] + abtRxBuf[offset + 1])) {
      // TODO: Retry
      log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Length checksum mismatch");
      pnd->iLastError = ECOMIO;
      return -1;
    }

    // abtRxBuf[3] (LEN) include TFI + (CC+1)
    len = abtRxBuf[offset] - 2;
    offset += 2;
  }

  if (len > szDataLen) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "Unable to receive data: buffer too small. (szDataLen: %zu, len: %zu)", szDataLen, len);
    pnd->iLastError = ECOMIO;
    return -1;
  }

  // TFI + PD0 (CC+1)
  if (abtRxBuf[offset] != 0xD5) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "TFI Mismatch");
    pnd->iLastError = ECOMIO;
    return -1;
  }
  offset += 1;

  if (abtRxBuf[offset] != CHIP_DATA (pnd)->ui8LastCommand + 1) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Command Code verification failed");
    pnd->iLastError = ECOMIO;
    return -1;
  }
  offset += 1;

  memcpy (pbtData, abtRxBuf + offset, len);
  offset += len;

  byte_t btDCS = (256 - 0xD5);
  btDCS -= CHIP_DATA (pnd)->ui8LastCommand + 1;
  for (size_t szPos = 0; szPos < len; szPos++) {
    btDCS -= pbtData[szPos];
  }

  if (btDCS != abtRxBuf[offset]) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Data checksum mismatch");
    pnd->iLastError = ECOMIO;
    return -1;
  }
  offset += 1;

  if (0x00 != abtRxBuf[offset]) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_ERROR, "%s", "Frame postamble mismatch");
    pnd->iLastError = ECOMIO;
    return -1;
  }
  // The PN53x command is done and we successfully received the reply
  pnd->iLastError = 0;
  return len;
}

int
pn53x_usb_ack (nfc_device_t * pnd)
{
  return pn53x_usb_bulk_write (DRIVER_DATA (pnd), (byte_t *) pn53x_ack_frame, sizeof (pn53x_ack_frame), NULL);
}

bool
pn53x_usb_init (nfc_device_t *pnd)
{
  // Sometimes PN53x USB doesn't reply ACK one the first frame, so we need to send a dummy one...
  //pn53x_check_communication (pnd); // Sony RC-S360 doesn't support this command for now so let's use a get_firmware_version instead:
  const byte_t abtCmd[] = { GetFirmwareVersion };
  pn53x_transceive (pnd, abtCmd, sizeof (abtCmd), NULL, NULL, NULL);
  // ...and we don't care about error
  pnd->iLastError = 0;
  if (SONY_RCS360 == DRIVER_DATA (pnd)->model) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_TRACE, "SONY RC-S360 initialization.");
    const byte_t abtCmd2[] = { 0x18, 0x01 };
    pn53x_transceive (pnd, abtCmd2, sizeof (abtCmd2), NULL, NULL, NULL);
    pn53x_usb_ack (pnd);
  }

  if (!pn53x_init (pnd))
    return false;

  if (ASK_LOGO == DRIVER_DATA (pnd)->model) {
    log_put (LOG_CATEGORY, NFC_PRIORITY_TRACE, "ASK LoGO initialization.");
    /* Internal registers */
    /* Disable 100mA current limit, Power on Secure IC (SVDD) */
    pn53x_write_register (pnd, PN53X_REG_Control_switch_rng, 0xFF, SYMBOL_CURLIMOFF | SYMBOL_SIC_SWITCH_EN | SYMBOL_RANDOM_DATAREADY);
    /* Select the signal to be output on SIGOUT: Modulation signal (envelope) from the internal coder */
    pn53x_write_register (pnd, PN53X_REG_CIU_TxSel, 0xFF, 0x14);

    /* SFR Registers */
    /* Setup push-pulls for pins from P30 to P35 */
    pn53x_write_register (pnd, PN53X_SFR_P3CFGB, 0xFF, 0x37);

/*
On ASK LoGO hardware:
  LEDs port bits definition: 
   * LED 1: bit 2 (P32)
   * LED 2: bit 1 (P31)
   * LED 3: bit 0 or 3 (depending of hardware revision) (P30 or P33)
   * LED 4: bit 5 (P35)
  Notes: 
   * Set logical 0 to switch LED on; logical 1 to switch LED off. 
   * Bit 4 should be maintained at 1 to keep RF field on.

  Progressive field activation:
   The ASK LoGO hardware can progressively power-up the antenna.
   To use this feature we have to switch on the field by switching on
   the field on PN533 (RFConfiguration) then set P34 to '1', and cut-off the 
   field by switching off the field on PN533 then set P34 to '0'.
*/

    /* Set P30, P31, P33, P35 to logic 1 and P32, P34 to 0 logic */
    /* ie. Switch LED1 on and turn off progressive field */
    pn53x_write_register (pnd, PN53X_SFR_P3, 0xFF, _BV (P30) | _BV (P31) | _BV (P33) | _BV (P35));
  }

  return true;
}

bool
pn53x_usb_configure (nfc_device_t * pnd, const nfc_device_option_t ndo, const bool bEnable)
{
  if (!pn53x_configure (pnd, ndo, bEnable))
    return false;

  switch (DRIVER_DATA (pnd)->model) {
    case ASK_LOGO:
      if (NDO_ACTIVATE_FIELD == ndo) {
        /* Switch on/off LED2 and Progressive Field GPIO according to ACTIVATE_FIELD option */
        log_put (LOG_CATEGORY, NFC_PRIORITY_TRACE, "Switch progressive field %s", bEnable ? "On" : "Off");
        if (!pn53x_write_register (pnd, PN53X_SFR_P3, _BV(P31) | _BV(P34), bEnable ? _BV (P34) : _BV (P31)))
          return false;
      }
      break;
    case SCM_SCL3711:
      if (NDO_ACTIVATE_FIELD == ndo) {
        // Switch on/off LED according to ACTIVATE_FIELD option
        if (!pn53x_write_register (pnd, PN53X_SFR_P3, _BV (P32), bEnable ? 0 : _BV (P32)))
          return false;
      }
      break;
    default:
      break;
  }
  return true;
}

bool
pn53x_usb_abort_command (nfc_device_t * pnd)
{
  DRIVER_DATA (pnd)->abort_flag = true;
  return true;
}

const struct pn53x_io pn53x_usb_io = {
  .send       = pn53x_usb_send,
  .receive    = pn53x_usb_receive,
};

const struct nfc_driver_t pn53x_usb_driver = {
  .name       = PN53X_USB_DRIVER_NAME,
  .probe      = pn53x_usb_probe,
  .connect    = pn53x_usb_connect,
  .disconnect = pn53x_usb_disconnect,
  .strerror   = pn53x_strerror,

  .initiator_init                   = pn53x_initiator_init,
  .initiator_select_passive_target  = pn53x_initiator_select_passive_target,
  .initiator_poll_target            = pn53x_initiator_poll_target,
  .initiator_select_dep_target      = pn53x_initiator_select_dep_target,
  .initiator_deselect_target        = pn53x_initiator_deselect_target,
  .initiator_transceive_bytes       = pn53x_initiator_transceive_bytes,
  .initiator_transceive_bits        = pn53x_initiator_transceive_bits,
  .initiator_transceive_bytes_timed = pn53x_initiator_transceive_bytes_timed,
  .initiator_transceive_bits_timed  = pn53x_initiator_transceive_bits_timed,

  .target_init           = pn53x_target_init,
  .target_send_bytes     = pn53x_target_send_bytes,
  .target_receive_bytes  = pn53x_target_receive_bytes,
  .target_send_bits      = pn53x_target_send_bits,
  .target_receive_bits   = pn53x_target_receive_bits,

  .configure  = pn53x_usb_configure,

  .abort_command  = pn53x_usb_abort_command,
  .idle  = pn53x_idle,
};
