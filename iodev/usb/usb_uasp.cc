/////////////////////////////////////////////////////////////////////////
// $Id$
/////////////////////////////////////////////////////////////////////////
//
//  Experimental USB Attached SCSI Protocol support (UASP)
//
//  Copyright (C) 2023  Benjamin D Lunt (fys [at] fysnet [dot] net)
//  Copyright (C) 2023  The Bochs Project
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
/////////////////////////////////////////////////////////////////////////

// Define BX_PLUGGABLE in files that can be compiled into plugins.  For
// platforms that require a special tag on exported symbols, BX_PLUGGABLE
// is used to know when we are exporting symbols and when we are importing.
#define BX_PLUGGABLE

#include "iodev.h"

#if BX_SUPPORT_PCI && BX_SUPPORT_PCIUSB
#include "usb_common.h"
#include "hdimage/cdrom.h"
#include "hdimage/hdimage.h"
#include "scsi_device.h"
#include "usb_msd.h"

#define LOG_THIS

// Information Unit Types
// byte[0] in uasp command and status????
#define IU_CMD   1
#define IU_SENSE 3
#define IU_RESP  4
#define IU_TMF   5
#define IU_RRDY  6
#define IU_WRDY  7

#define IU_CMD_LEN    32
#define IU_SENSE_LEN  16
#define IU_RESP_LEN    8
#define IU_TMF_LEN    16
#define IU_RRDY_LEN    4
#define IU_WRDY_LEN    4 

// Task Attribute
#define UASP_TASK_SIMPLE  0
#define UASP_TASK_HOQ     1
#define UASP_TASK_ORDERED 2
#define UASP_TASK_ACA     4

#define UASP_GET_ACTIVE(m)   (((m) &  1) > 0) // is the request active?
#define UASP_SET_ACTIVE(m)    ((m) > 0)       // clearing/setting active needs to clear everything else
#define UASP_GET_STATUS(m)   (((m) &  2) > 0) // have we sent the RRIU or WRIU?
#define UASP_SET_STATUS(m)    ((m) |  2)      // set that we have sent the RRIU or WRIU
#define UASP_GET_CMND(m)     (((m) &  4) > 0) // have we received and processed the command yet?
#define UASP_SET_CMND(m)      ((m) |  4)      // set that we have received and processed the command
#define UASP_GET_COMPLETE(m) (((m) &  8) > 0) // is the datain/dataout transfer complete?
#define UASP_SET_COMPLETE(m)  ((m) |  8)      // set that the datain/dataout transfer is complete
#define UASP_GET_RESPONSE(m) (((m) & 16) > 0) // is there a response ready?
#define UASP_SET_RESPONSE(m)  ((m) | 16)      // set that there is a response ready
#define UASP_GET_DIR(m)      (((m) &  0x0000FF00) >> 8)
#define UASP_SET_DIR(m,d)    (((m) & ~0x0000FF00) | ((d) << 8))

struct S_UASP_HDR {
  Bit8u  id;         // Information Unit Type
  Bit8u  rsvd0;      //
  Bit16u tag;        // big endian
};

struct S_UASP_COMMAND {
  Bit8u  id;         // Information Unit Type
  Bit8u  rsvd0;      //
  Bit16u tag;        // big endian
  Bit8u  prio_attr;  // Task Attribute
  Bit8u  rsvd1;      //
  Bit8u  len;        // len (if more than 16 bytes) (i.e.: if command len is 20 bytes, this field is 4)  (bottom 2 bits are reserved)
  Bit8u  rsvd2;      //
  Bit64u lun;        // eight byte lun
  Bit8u  com_block[16]; // command block
};

struct S_UASP_STATUS {
  Bit8u  id;         // usually IU_SENSE ?
  Bit8u  rsvd0;
  Bit16u tag;        // big endian
  Bit16u stat_qual;  // big endian
  Bit8u  status;
  Bit8u  resv1[7];
  Bit16u len;        // big endian
  Bit8u  sense[18];
};

#define IU_TMF_ABORT        0x01
#define IU_TMF_ABORT_SET    0x02
#define IU_TMF_CLEAR        0x04
#define IU_TMF_RESET_LUN    0x08
#define IU_TMF_NEXUS_RESET  0x10
#define IU_TMF_CLEAR_ACA    0x40
#define IU_TMF_QUERY        0x80
#define IU_TMF_QUERY_SET    0x81
#define IU_TMF_QUERY_ASYNC  0x82
struct S_UASP_TASK_MAN {
  Bit8u  id;         // 0x05
  Bit8u  rsvd0;      // 
  Bit16u tag;        // big endian
  Bit8u  function;   // funtion
  Bit8u  rsvd1;      // 
  Bit16u task_tag;   // tag of task to be managed
  Bit64u lun;        // eight byte lun
};

struct S_UASP_RESPONSE {
  Bit8u  id;            // 0x04
  Bit8u  rsvd0;         // 
  Bit16u tag;           // big endian
  Bit8u  additional[3]; // additional data
  Bit8u  response;      // response code
};

#define UASP_FROM_COMMAND  ((Bit32u) -1)
#define U_NONE          0
#define U_SRV_ACT   (1<<0)
#define U_IS_LBA    (1<<1)
struct S_UASP_INPUT {
  Bit8u  command;      // command byte
  Bit8u  serv_action;  // command/service action
  Bit8u  cmd_len;      // length of the command buf
  Bit8u  direction;    // in or out
  Bit8u  flags;        // see S_UASP_FLAGS_*
  Bit32u data_len;     // if the command is fixed at a count of bytes, this holds that count (next two members are ignored)
  int    offset;       // byte offset of data length requested
  int    size;         // size in bytes of this field
};

int usb_msd_device_c::uasp_handle_data(USBPacket *p)
{
  int ret = 0;
  Bit8u *data = p->data;
  int len = p->len;
  int index = p->strm_pid; // high-speed device will be zero. super-speed device will have the stream id.
  
  BX_DEBUG(("uasp_handle_data(): %X  ep=%d  index=%d  len=%d", p->pid, p->devep, index, len));
  
  switch (p->pid) {
    case USB_TOKEN_OUT:
      if (p->devep == MSD_UASP_DATAOUT) { // Bulk out pipe
        if ((index < 0) || (index > UASP_MAX_STREAMS_N))
          goto fail;
        ret = uasp_process_request(p, index);
        if (ret == USB_RET_ASYNC)
          usb_defer_packet(p, this);
      } else if (p->devep == MSD_UASP_COMMAND) { // Command pipe
        ret = uasp_do_command(p);
      }
      break;
      
    case USB_TOKEN_IN:
      // TODO: these are the same
      if ((p->devep == MSD_UASP_DATAIN) || // Bulk in pipe
          (p->devep == MSD_UASP_STATUS)) { // Status pipe
        if ((index < 0) || (index > UASP_MAX_STREAMS_N))
          goto fail;
        ret = uasp_process_request(p, index);
        if (ret == USB_RET_ASYNC)
          usb_defer_packet(p, this);
      }
      break;
      
    default:
      BX_ERROR(("USB MSD (UASP) handle_data: bad token"));
fail:
      d.stall = 1;
      ret = USB_RET_STALL;
  }

  return ret;
}

void usb_msd_device_c::uasp_initialize_request(int index)
{
  UASPRequest *req = &s.uasp_request[index];

  req->mode = UASP_SET_ACTIVE(1);  // setting to 1 clears other flags
  req->data_len = 0;
  req->result = 0;
  req->tag = 0;
  req->scsi_len = 0;
  req->status = NULL;  
  req->lun = 0;
  req->p = NULL;

  d.stall = 0;
}

// if tag != 0xFFFFFFFF, use the tag to find the request
// if lun != 0xFF, use the lun to find the request
// if both are not 0xFFFFFF/0xFF, use them both
UASPRequest *usb_msd_device_c::uasp_find_request(Bit32u tag, Bit8u lun)
{
  UASPRequest *req = NULL;

  if (tag != 0xFFFFFFFF) {
    for (int i=0; i<=UASP_MAX_STREAMS_N; i++) {
      if (UASP_GET_ACTIVE(s.uasp_request[i].mode) && (s.uasp_request[i].tag == tag)) {
        req = &s.uasp_request[i];
        break;
      }
    }
  }

  if (lun != 0xFF) {
    for (int i=0; i<=UASP_MAX_STREAMS_N; i++) {
      if (UASP_GET_ACTIVE(s.uasp_request[i].mode) && (s.uasp_request[i].lun == lun)) {
        if (req == NULL) {
          req = &s.uasp_request[i];
          break;
        } else {
          if (req == &s.uasp_request[i]) {
            break;
          } else {
            req = NULL;
            break;
          }
        }
      }
    }
  }

  return req;
}

const struct S_UASP_INPUT bx_uasp_info_array[] = {
  // cmd, serv, cmd_len, direction,  flags,      data_len, (offset, size)
// SPC-4 commands
  { 0x12, 0x00,    6, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  3, 2 },  // INQUIRY
  { 0x15, 0x00,    6, USB_TOKEN_OUT, U_NONE,    UASP_FROM_COMMAND,  4, 1 },  // MODE_SELECT_6
  { 0x55, 0x00,   10, USB_TOKEN_OUT, U_NONE,    UASP_FROM_COMMAND,  7, 2 },  // MODE_SELECT_10
  { 0x1A, 0x00,    6, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  4, 1 },  // MODE_SENSE_6
  { 0x5A, 0x00,   10, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  7, 2 },  // MODE_SENSE_10
  { 0xA0, 0x00,   12, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  6, 4 },  // REPORT LUNS
  { 0x03, 0x00,    6, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  4, 1 },  // REQUEST_SENSE
  { 0x1D, 0x00,    6, USB_TOKEN_OUT, U_NONE,    UASP_FROM_COMMAND,  3, 2 },  // SEND_DIAGNOSTIC
  { 0x00, 0x00,    6, 0,             U_NONE,                    0,       },  // TEST_UNIT_READY
// SBC-3 commands
  { 0x08, 0x00,    6, USB_TOKEN_IN,  U_IS_LBA,  UASP_FROM_COMMAND,  2, 2 },   // READ_6
  { 0x28, 0x00,   10, USB_TOKEN_IN,  U_IS_LBA,  UASP_FROM_COMMAND,  7, 2 },   // READ_10
  { 0xA8, 0x00,   12, USB_TOKEN_IN,  U_IS_LBA,  UASP_FROM_COMMAND,  6, 4 },   // READ_12
  { 0x88, 0x00,   16, USB_TOKEN_IN,  U_IS_LBA,  UASP_FROM_COMMAND, 10, 4 },   // READ_16
  { 0x0A, 0x00,    6, USB_TOKEN_OUT, U_IS_LBA,  UASP_FROM_COMMAND,  2, 2 },   // WRITE_6
  { 0x2A, 0x00,   10, USB_TOKEN_OUT, U_IS_LBA,  UASP_FROM_COMMAND,  7, 2 },   // WRITE_10
  { 0xAA, 0x00,   12, USB_TOKEN_OUT, U_IS_LBA,  UASP_FROM_COMMAND,  6, 4 },   // WRITE_12
  { 0x8A, 0x00,   16, USB_TOKEN_IN,  U_IS_LBA,  UASP_FROM_COMMAND, 10, 4 },   // WRITE_16
  { 0x1E, 0x00,    6, 0,             U_NONE,                    0,       },   // PREVENT_ALLOW_MEDIUM_REMOVAL
  { 0x25, 0x00,   10, USB_TOKEN_IN,  U_NONE,                    8,       },   // READ_CAPACITY_10
  { 0x9E, 0x10,   16, USB_TOKEN_IN,  U_SRV_ACT, UASP_FROM_COMMAND, 10, 4 },   // READ_CAPACITY_16
  { 0x1B, 0x00,    6, 0,             U_NONE,                    0,       },   // START_STOP_UNIT
  { 0x2F, 0x00,   10, 0,             U_NONE,                    0,       },   // VERIFY_10
  { 0xAF, 0x00,   12, 0,             U_NONE,                    0,       },   // VERIFY_12
  { 0x8F, 0x00,   16, 0,             U_NONE,                    0,       },   // VERIFY_16
  { 0x04, 0x00,    6, 0,             U_NONE,                    0,       },   // FORMAT_UNIT
  { 0x35, 0x00,   10, 0,             U_NONE,                    0,       },   // SYNCHRONIZE CACHE_10
  { 0x91, 0x00,   16, 0,             U_NONE,                    0,       },   // SYNCHRONIZE CACHE_16
// MMC-6
  // MMC-6r2f, section 5.3.2, page 202(250) states all commands we must support (see section 5.3.3 also)
  { 0x46, 0x00,   10, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  7, 2 },   // GET_CONFIG
  { 0x4A, 0x00,   10, USB_TOKEN_IN,  U_NONE,    UASP_FROM_COMMAND,  7, 2 },   // GET EVENT STATUS NOTIFICATION
// end marker
  { 0xFF, }
};

Bit32u usb_msd_device_c::get_data_len(const struct S_UASP_INPUT *input, Bit8u *buf)
{
  Bit32u ret = 0;

  // the input offset and size fields point to the commands "data_len" field
  switch (input->size) {
    case 1: // byte
      ret = * (Bit8u *) &buf[input->offset];
      break;
    case 2: // word
      ret = * (Bit16u *) &buf[input->offset];
      ret = bx_bswap16(ret);
      break;
    case 4: // dword
      ret = * (Bit32u *) &buf[input->offset];
      ret = bx_bswap32(ret);
      break;
  }

  // is lba instead of bytes?
  if (input->flags & U_IS_LBA)
    ret *= s.sect_size;

  return ret;
}

// get information about the command requested
const struct S_UASP_INPUT *usb_msd_device_c::uasp_get_info(Bit8u command, Bit8u serv_action)
{
  int i = 0;

  do {
    if (bx_uasp_info_array[i].command == command) {
      if (bx_uasp_info_array[i].flags & U_SRV_ACT) {
        if (bx_uasp_info_array[i].serv_action == serv_action)
          return &bx_uasp_info_array[i];
      } else
        return &bx_uasp_info_array[i];
    }
    i++;
  } while (bx_uasp_info_array[i].command != 0xFF);

  BX_ERROR(("uasp_get_info: Unknown command found: 0x%02X(0x%02X)", command, serv_action));
  return NULL;
}

int usb_msd_device_c::uasp_do_stall(UASPRequest *req)
{
  USBPacket *p = req->p;

  if (p) {
    req->p = NULL;
    p->len = USB_RET_STALL;
    usb_packet_complete(p);
  }

  p = req->status;
  if (p) {
    req->status = NULL;
    p->len = USB_RET_STALL;
    usb_packet_complete(p);
  }

  req->mode = UASP_SET_ACTIVE(0);
  d.stall = 1;
  return USB_RET_STALL;
}

int usb_msd_device_c::uasp_do_command(USBPacket *p)
{
  const struct S_UASP_INPUT *input;
  const Bit8u lun = p->data[15];  // the lun is in the same place for a IU_CMD and IU_TMF
  struct S_UASP_HDR *hdr = (struct S_UASP_HDR *) p->data;
  const int index = (get_speed() == USB_SPEED_HIGH) ? 0 : bx_bswap16(hdr->tag);
  UASPRequest *req = &s.uasp_request[index];
  Bit8u cmd_len = 0;
  
  usb_dump_packet(p->data, p->len, 0, p->devaddr, USB_DIR_OUT | p->devep, USB_TRANS_TYPE_BULK, false, true);
  if (hdr->id == IU_CMD) {
    struct S_UASP_COMMAND *iu = (struct S_UASP_COMMAND *) p->data;
    if ((iu->prio_attr & 0x7) == UASP_TASK_SIMPLE) {
      if (!UASP_GET_ACTIVE(req->mode))
        uasp_initialize_request(index);
      // get information about the command requested
      input = uasp_get_info(iu->com_block[0], iu->com_block[1] & 0x1F);
      // get the count of bytes requested, command length, etc.
      req->tag = bx_bswap16(iu->tag);
      if (input != NULL) {
        req->mode = UASP_SET_DIR(req->mode, input->direction);
        req->data_len = (input->data_len == UASP_FROM_COMMAND) ? get_data_len(input, iu->com_block) : input->data_len;
        cmd_len = input->cmd_len;
      } else {
        // Since this is a command we don't support, the scsi should return SENSE_ILLEGAL_REQUEST
        req->data_len = 0;
        cmd_len = 0;
      }
      req->lun = lun;
      BX_DEBUG(("uasp command id %d, tag 0x%04X, command 0x%X, len = %d, data_len = %d", iu->id, req->tag, iu->com_block[0], p->len, req->data_len));
      
      s.scsi_dev->scsi_send_command(req->tag, iu->com_block, cmd_len, lun, d.async_mode);
      if (!UASP_GET_COMPLETE(req->mode)) { // zero transfer command?
        if (UASP_GET_DIR(req->mode) == USB_TOKEN_IN) {
          s.scsi_dev->scsi_read_data(req->tag);
        } else if (UASP_GET_DIR(req->mode) == USB_TOKEN_OUT) {
          s.scsi_dev->scsi_write_data(req->tag);
        }
      }
      
      // if a high-speed device, we need to send the status ready iu
      if ((get_speed() == USB_SPEED_HIGH) && req->status) {
        USBPacket *s = req->status;
        s->len = uasp_do_ready(req, s);
        req->status = NULL;
        usb_packet_complete(s);
      }
      
      req->mode = UASP_SET_CMND(req->mode); // mark that we have sent the command
      return p->len;
    } else
      BX_ERROR(("uasp: unknown/unsupported task attribute. %d", (iu->prio_attr & 0x7)));
    
  } else if (hdr->id == IU_TMF) {
    
    BX_ERROR(("USAP: Task Management is not fully functional yet"));

    struct S_UASP_TASK_MAN *tmiu = (struct S_UASP_TASK_MAN *) p->data;
    UASPRequest *treq;
    switch (tmiu->function) {
      case IU_TMF_ABORT:
        treq = uasp_find_request(bx_bswap16(tmiu->task_tag), lun);
        //req->result = 0;
        break;
      case IU_TMF_ABORT_SET:
        treq = uasp_find_request(0xFFFFFFFF, lun);
        //req->result = 0;
        break;
      case IU_TMF_RESET_LUN:
        treq = uasp_find_request(0xFFFFFFFF, lun);
        BX_DEBUG(("IU_TMF_RESET_LUN: tag = 0x%04X", treq->tag));
        if (treq != NULL) {
          s.scsi_dev->scsi_cancel_io(treq->tag);
          req->result = STATUS_GOOD;
        } else
          BX_ERROR(("IU_TMF_RESET_LUN: Did not find request for lun %i", lun));
        break;
      case IU_TMF_NEXUS_RESET:
        //req->result = 0;
        break;
      case IU_TMF_CLEAR_ACA:
        treq = uasp_find_request(0xFFFFFFFF, lun);
        //req->result = 0;
        break;
      case IU_TMF_QUERY:
        treq = uasp_find_request(bx_bswap16(tmiu->task_tag), lun);
        //req->result = 0;
        break;
      case IU_TMF_QUERY_SET:
        treq = uasp_find_request(0xFFFFFFFF, lun);
        //req->result = 0;
        break;
      case IU_TMF_QUERY_ASYNC:
        treq = uasp_find_request(0xFFFFFFFF, lun);
        //req->result = 0;
        break;
      default:
        BX_ERROR(("uasp: unknown TMF function number: %d", tmiu->function));
    }
    // do the response
    USBPacket *r = req->status;
    if (r) {
      r->len = uasp_do_response(req, r);
      req->status = NULL;
      usb_packet_complete(r);
    } else
      UASP_SET_RESPONSE(req->mode);
    return p->len;
  } else
    BX_ERROR(("uasp: unknown IU_id on command pipe: %d", hdr->id));

  return 0;
}

// if we have already done the command and there is data
//  waiting, we can process the packet request.
// else ret USB_RET_ASYNC
int usb_msd_device_c::uasp_process_request(USBPacket *p, int index)
{
  UASPRequest *req = &s.uasp_request[index];
  int len = p->len;

  if (!UASP_GET_ACTIVE(req->mode))
    uasp_initialize_request(index);

  // if it is the status pipe
  if (p->devep == MSD_UASP_STATUS) {
    // TODO: check to see if the direction is in
    if (UASP_GET_COMPLETE(req->mode)) {
      return uasp_do_status(req, p);
    } else if (UASP_GET_RESPONSE(req->mode)) {
      return uasp_do_response(req, p);
    } else {
      if ((get_speed() == USB_SPEED_HIGH) && UASP_GET_CMND(req->mode) && !UASP_GET_STATUS(req->mode)) {
        return uasp_do_ready(req, p);
      } else {
        req->status = p;
        return USB_RET_ASYNC;
      }
    }
  }
  
  // have we received the Command yet?
  if (!UASP_GET_CMND(req->mode)) {
    req->p = p;
    return USB_RET_ASYNC;
  }

  // check to make sure the direction is correct
  if (p->pid != UASP_GET_DIR(req->mode)) {
    BX_ERROR(("Found packet with wrong direction."));
    uasp_do_stall(req);
  }
  
  // do the transfer for this packet
  len = uasp_do_data(req, p);
  BX_DEBUG(("uasp: data: transferred %d bytes", len));
  
  return len;
}

int usb_msd_device_c::uasp_do_data(UASPRequest *req, USBPacket *p)
{
  int len = p->len;

  // TODO: if (dir != req->dir) error
  if (UASP_GET_DIR(req->mode) == USB_TOKEN_IN) {
    BX_DEBUG(("data in %d/%d/%d", len, req->data_len, req->scsi_len));
  } else if (UASP_GET_DIR(req->mode) == USB_TOKEN_OUT) {
    BX_DEBUG(("data out %d/%d/%d", len, req->data_len, req->scsi_len));
  }
  
  if (len > (int) req->scsi_len)
    len = req->scsi_len;
  req->usb_buf = p->data;
  req->usb_len = len;
  while (req->usb_len && req->scsi_len) {
    uasp_copy_data(req);
  }
  
  if (req->residue && req->usb_len) {
    req->data_len -= req->usb_len;
    memset(req->usb_buf, 0, req->usb_len);
    req->usb_len = 0;
  }
  
  // This fills the log.txt file extremely fast for sector transfers
  // Therefore, I only dump packets that are less than sector transfers
  if (len < (int) s.sect_size)
    usb_dump_packet(p->data, len, 0, p->devaddr, ((UASP_GET_DIR(req->mode) == USB_TOKEN_IN) ? USB_DIR_IN : USB_DIR_OUT) | p->devep, USB_TRANS_TYPE_BULK, false, true);

  return len;
}

int usb_msd_device_c::uasp_do_ready(UASPRequest *req, USBPacket *p)
{
  struct S_UASP_HDR *ready;
  
  // do the RRIU or WRIU
  ready = (struct S_UASP_HDR *) p->data;
  ready->id = (UASP_GET_DIR(req->mode) == USB_TOKEN_IN) ? IU_RRDY : IU_WRDY;
  ready->rsvd0 = 0;
  ready->tag = bx_bswap16((Bit16u) req->tag);
  usb_dump_packet(p->data, IU_RRDY_LEN, 0, p->devaddr, USB_DIR_IN | p->devep, USB_TRANS_TYPE_BULK, false, true);
  
  req->mode = UASP_SET_STATUS(req->mode);

  return IU_RRDY_LEN;
}

int usb_msd_device_c::uasp_do_status(UASPRequest *req, USBPacket *p)
{
  struct S_UASP_STATUS *status;
  int ret = IU_SENSE_LEN;
  
  BX_DEBUG(("uasp: Sending Status:"));
  if (p->len < IU_SENSE_LEN)
    BX_ERROR(("Status packet length is less than 16: %d", p->len));
  
  // do the status
  status = (struct S_UASP_STATUS *) p->data;
  memset(status, 0, IU_SENSE_LEN);
  status->id = IU_SENSE;
  status->tag = bx_bswap16((Bit16u) req->tag);
  status->status = (Bit8u) req->result;
  status->len = bx_bswap16(0); // assume no sense data
  if (req->result == STATUS_GOOD) {
    // nothing
  } else if (req->result == STATUS_CHECK_CONDITION) {
    status->stat_qual = 0;
    if (p->len >= sizeof(struct S_UASP_STATUS)) {
      // do a REQUEST SENSE command
      static Bit8u request_sense[6] = { 0x03, 0, 0, 0, 18, 0 };
      UASPRequest *r = &s.uasp_request[UASP_MAX_STREAMS_N];
      uasp_initialize_request(UASP_MAX_STREAMS_N);
      r->tag = UASP_MAX_STREAMS_N;
      s.scsi_dev->scsi_send_command(r->tag, request_sense, 6, 0, 0);
      s.scsi_dev->scsi_read_data(r->tag);
      r->mode = UASP_SET_DIR(r->mode, USB_TOKEN_IN);
      r->usb_len = 18;
      r->usb_buf = status->sense;
      uasp_copy_data(r);
      r->mode = UASP_SET_ACTIVE(0);
      ret += 18;
      status->len = bx_bswap16(18);
    }
  } else {
    BX_ERROR(("uasp: Unknown command completion status: %d", req->result));
  }
  
  usb_dump_packet(p->data, ret, 0, p->devaddr, USB_DIR_IN | p->devep, USB_TRANS_TYPE_BULK, false, true);
  
  req->mode = UASP_SET_ACTIVE(0);
  
  // return the size of the status block
  return ret;
}

int usb_msd_device_c::uasp_do_response(UASPRequest *req, USBPacket *p)
{
  struct S_UASP_RESPONSE *response;
  
  // do the response
  BX_DEBUG(("uasp: Sending Response:"));
  response = (struct S_UASP_RESPONSE *) p->data;
  memset(response, 0, IU_RESP_LEN);
  response->id = IU_RESP;
  response->tag = bx_bswap16((Bit16u) req->tag);
  // addtional response goes here
  response->response = (Bit8u) req->result;
  usb_dump_packet(p->data, IU_RESP_LEN, 0, p->devaddr, USB_DIR_IN | p->devep, USB_TRANS_TYPE_BULK, false, true);
  
  req->mode = UASP_SET_ACTIVE(0);
  
  // return the size of the status block
  return IU_RESP_LEN;
}

void usb_msd_device_c::uasp_copy_data(UASPRequest *req)
{
  Bit32u len = req->usb_len;
  if (len > req->scsi_len)
    len = req->scsi_len;
  if (UASP_GET_DIR(req->mode) == USB_TOKEN_IN) {
    memcpy(req->usb_buf, req->scsi_buf, len);
  } else {
    memcpy(req->scsi_buf, req->usb_buf, len);
  }
  req->usb_len -= len;
  req->scsi_len -= len;
  req->usb_buf += len;
  req->scsi_buf += len;
  req->data_len -= len;
  if (req->scsi_len == 0) {
    if (UASP_GET_DIR(req->mode) == USB_TOKEN_IN) {
      s.scsi_dev->scsi_read_data(req->tag);
    } else {
      s.scsi_dev->scsi_write_data(req->tag);
    }
  }
}

// reason = SCSI_REASON_DONE or SCSI_REASON_DATA
// tag = tag of scsi request
// if reason == SCSI_REASON_DONE
//   arg = STATUS_GOOD or STATUS_CHECK_CONDITION
//   sense = 0 or sense of transfer
// if reason == SCSI_REASON_DATA
//   arg = bytes transferred
//   sense = 0;
void usb_msd_device_c::uasp_command_complete(int reason, Bit32u tag, Bit32u arg)
{
  USBPacket *p;
  UASPRequest *req = uasp_find_request(tag, 0xFF);

  BX_DEBUG(("uasp_command_complete: reason %d, arg %d, tag 0x%04X", reason, arg, tag));

  if (req == NULL) {
    BX_ERROR(("uasp_command_complete: Tag 0x%X not found."));
    return;
  }

  if (reason == SCSI_REASON_DONE) {
    req->residue = req->data_len;
    req->result = arg;  // STATUS_GOOD = 0 or STATUS_CHECK_CONDITION = 2
    req->mode = UASP_SET_COMPLETE(req->mode); // mark that are transfer is complete
    // do the status
    p = req->status;
    if (p) {
      p->len = uasp_do_status(req, p);
      BX_DEBUG(("uasp: status: transferred %d bytes (residue = %d)", p->len, req->residue));
      req->status = NULL;
      usb_packet_complete(p);
    }
    return;
  }

  // reason == SCSI_REASON_DATA
  req->scsi_len = arg;  // bytes transferred
  req->scsi_buf = s.scsi_dev->scsi_get_buf(tag);
  p = req->p;
  if (p) {
    p->len = uasp_do_data(req, p);
    BX_DEBUG(("uasp: transferred %d bytes", p->len));
    BX_DEBUG(("packet complete 0x%p", p));
    req->p = NULL;
    usb_packet_complete(p);
  }
}

#endif // BX_SUPPORT_PCI && BX_SUPPORT_PCIUSB
