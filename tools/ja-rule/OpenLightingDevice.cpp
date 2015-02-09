/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * OpenLightingDevice.cpp
 * The Open Lighting USB Device.
 * Copyright (C) 2015 Simon Newton
 */

#include "tools/ja-rule/OpenLightingDevice.h"

#include <libusb.h>

#include <string.h>

#include <ola/Callback.h>
#include <ola/Clock.h>
#include <ola/Constants.h>
#include <ola/DmxBuffer.h>
#include <ola/Logging.h>
#include <ola/base/Array.h>
#include <ola/io/SelectServer.h>
#include <ola/strings/Format.h>
#include <ola/thread/Mutex.h>

#include <memory>
#include <string>

using std::auto_ptr;
using std::cout;
using std::string;

using ola::Clock;
using ola::NewCallback;
using ola::NewSingleCallback;
using ola::TimeInterval;
using ola::TimeStamp;
using ola::io::SelectServer;
using ola::thread::Mutex;
using ola::thread::MutexLocker;

static const uint8_t kInEndpoint = 0x81;
static const uint8_t kOutEndpoint = 0x01;
static const unsigned int kTimeout = 1000;

namespace {

void InTransferCompleteHandler(struct libusb_transfer *transfer) {
  OpenLightingDevice *sender = static_cast<OpenLightingDevice*>(
      transfer->user_data);
  return sender->_InTransferComplete();
}

void OutTransferCompleteHandler(struct libusb_transfer *transfer) {
  OpenLightingDevice *sender = static_cast<OpenLightingDevice*>(
      transfer->user_data);
  return sender->_OutTransferComplete();
}
}  // namespace


OpenLightingDevice::OpenLightingDevice(SelectServer* ss, libusb_device* device)
  : m_ss(ss),
    m_device(device),
    m_handle(NULL),
    m_out_transfer(libusb_alloc_transfer(0)),
    m_out_in_progress(false),
    m_in_transfer(libusb_alloc_transfer(0)),
    m_in_in_progress(false) {
}

OpenLightingDevice::~OpenLightingDevice() {
  {
    MutexLocker locker(&m_out_mutex);
    if (m_out_in_progress) {
      OLA_INFO << "cancel m_out_transfer";
      libusb_cancel_transfer(m_out_transfer);
    }
  }

  {
    MutexLocker locker(&m_in_mutex);
    if (m_in_in_progress) {
      OLA_INFO << "cancel m_in_transfer";
      libusb_cancel_transfer(m_in_transfer);
    }
  }

  OLA_INFO << "Waiting for out to complete";
  while (true) {
    MutexLocker locker(&m_out_mutex);
    if (!m_out_in_progress) {
      break;
    }
  }

  OLA_INFO << "Waiting for in to complete";
  while (true) {
    MutexLocker locker(&m_in_mutex);
    if (!m_in_in_progress) {
      break;
    }
  }

  if (m_out_transfer) {
    libusb_free_transfer(m_out_transfer);
  }

  if (m_in_transfer) {
    libusb_free_transfer(m_in_transfer);
  }

  if (m_handle) {
    libusb_close(m_handle);
    m_handle = NULL;
  }
}

bool OpenLightingDevice::Init() {
  int r = libusb_open(m_device, &m_handle);
  if (r != 0) {
    OLA_WARN << "Failed to open device: " << libusb_error_name(r);
    return false;
  }

  r = libusb_claim_interface(m_handle, 0);
  if (r) {
    OLA_WARN << "Failed to claim interface: 0";
    libusb_close(m_handle);
    return false;
  }
  return true;
}

bool OpenLightingDevice::SendMessage(Command command,
                                     const uint8_t *data,
                                     unsigned int size) {
  if (size > MAX_PAYLOAD_SIZE) {
    OLA_WARN << "Message exceeds max payload size";
    return false;
  }

  MutexLocker locker(&m_out_mutex);
  if (m_out_in_progress) {
    OLA_WARN << "TX in progress, dropping message";
    return false;
  }

  unsigned int offset = 0;
  m_out_buffer[0] = SOF_IDENTIFIER;
  m_out_buffer[1] = static_cast<uint8_t>(command & 0xff);
  m_out_buffer[2] = static_cast<uint8_t>(command >> 8);
  m_out_buffer[3] = static_cast<uint8_t>(size & 0xff);
  m_out_buffer[4] = static_cast<uint8_t>(size >> 8);
  offset += 5;

  if (size > 0) {
    memcpy(m_out_buffer + offset, data, size);
    offset += size;
  }
  m_out_buffer[offset++] = EOF_IDENTIFIER;

  if (offset % USB_PACKET_SIZE == 0)  {
    // We need to pad the message so that the transfer completes on the
    // Device side. We could use LIBUSB_TRANSFER_ADD_ZERO_PACKET instead but
    // that isn't avaiable on all platforms.
    m_out_buffer[offset++] = 0;
  }

  libusb_fill_bulk_transfer(m_out_transfer, m_handle, kOutEndpoint,
                            m_out_buffer, offset,
                            OutTransferCompleteHandler,
                            static_cast<void*>(this),
                            kTimeout);

  Clock clock;
  clock.CurrentTime(&m_out_sent_time);
  OLA_INFO << "TX: Sending " << offset << " bytes";

  int r = libusb_submit_transfer(m_out_transfer);

  if (r) {
    OLA_WARN << "Failed to submit out transfer: " << libusb_error_name(r);
    return false;
  }

  m_out_in_progress = true;
  locker.Release();
  // Submit the In transfer here to reduce the latency.
  SubmitInTransfer();
  return true;
}

void OpenLightingDevice::_OutTransferComplete() {
  if (m_out_transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    if (m_out_transfer->actual_length != m_out_transfer->length) {
      OLA_WARN << "Only sent " << m_out_transfer->actual_length << " / "
               << m_out_transfer->length << " bytes";
    }
  }
  {
    MutexLocker locker(&m_out_mutex);
    m_out_in_progress = false;
  }
}

void OpenLightingDevice::_InTransferComplete() {
  TimeStamp now;
  Clock clock;
  clock.CurrentTime(&now);
  OLA_INFO << "Command completed in " << (now - m_out_sent_time)
           << ", status is " << libusb_error_name(m_in_transfer->status);

  if (m_in_transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    // Ownership of the buffer is transferred to the HandleData method,
    // running on the SS thread.
    m_ss->Execute(
        NewSingleCallback(
          this, &OpenLightingDevice::HandleData,
          reinterpret_cast<const uint8_t*>(m_in_transfer->buffer),
          static_cast<unsigned int>(m_in_transfer->actual_length)));
  } else {
    delete[] m_in_transfer->buffer;
  }

  {
    MutexLocker locker(&m_out_mutex);
    m_in_in_progress = false;
  }
}

bool OpenLightingDevice::SubmitInTransfer() {
  MutexLocker locker(&m_in_mutex);
  if (m_in_in_progress) {
    OLA_WARN << "Reading already pending";
    return true;
  }


  uint8_t* rx_buffer = new uint8_t[IN_BUFFER_SIZE];
  libusb_fill_bulk_transfer(m_in_transfer, m_handle, kInEndpoint,
                            rx_buffer, IN_BUFFER_SIZE,
                            InTransferCompleteHandler,
                            static_cast<void*>(this),
                            kTimeout);

  Clock clock;
  clock.CurrentTime(&m_send_in_time);
  int r = libusb_submit_transfer(m_in_transfer);
  if (r) {
    OLA_WARN << "Failed to submit input transfer: " << libusb_error_name(r);
    return false;
  }

  m_in_in_progress = true;
  return true;
}

void OpenLightingDevice::HandleData(const uint8_t* data, unsigned int size) {
  class ArrayDeleter {
   public:
    explicit ArrayDeleter(const uint8_t* data) : m_data(data) {}
    ~ArrayDeleter() { delete[] m_data; }
   private:
    const uint8_t* m_data;
  };

  ArrayDeleter deleter(data);

  // Right now we assume that the device only sends a single message at a time.
  // If this ever change from a message model to more of a stream model we'll
  // need to fix this.
  if (!m_message_handler) {
    return;
  }

  if (size < MIN_RESPONSE_SIZE) {
    OLA_WARN << "Response was too small, " << size << " bytes, min was "
             << MIN_RESPONSE_SIZE;
    return;
  }

  if (data[0] != SOF_IDENTIFIER) {
    OLA_WARN << "SOF mismatch, was 0x" << ola::strings::ToHex(data[0]);
    return;
  }

  uint16_t command = data[1] + (data[2] << 8);
  uint16_t payload_size = data[3] + (data[4] << 8);
  uint8_t return_code = data[5];
  uint8_t flags = data[6];

  if (payload_size + MIN_RESPONSE_SIZE > size) {
    OLA_WARN << "Message size of " << (payload_size + MIN_RESPONSE_SIZE)
             << " is greater than rx size of " << size;
    return;
  }

  ola::strings::FormatData(&std::cout, data, size);

  if (data[MIN_RESPONSE_SIZE + payload_size - 1] != EOF_IDENTIFIER) {
    OLA_WARN << "EOF_IDENTIFIER mismatch, was "
             << ola::strings::ToHex(data[payload_size + MIN_RESPONSE_SIZE - 1]);
    return;
  }

  MessageHandlerInterface::Message message = {
    command, return_code, flags,
    data + MIN_RESPONSE_SIZE - 1, payload_size
  };

  m_message_handler->NewMessage(message);
}
