/*
 * usb-ccid.c -- USB CCID protocol handling
 *
 * Copyright (C) 2010, 2011, 2012, 2013, 2014, 2015, 2016
 *               Free Software Initiative of Japan
 * Author: NIIBE Yutaka <gniibe@fsij.org>
 *
 * This file is a part of Gnuk, a GnuPG USB Token implementation.
 *
 * Gnuk is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Gnuk is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
 * License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>
#include <string.h>
#include <chopstx.h>
#include <eventflag.h>

#include "config.h"

#ifdef DEBUG
#include "debug.h"
struct stdout stdout;
#endif

#include "gnuk.h"
#include "usb_lld.h"
#include "usb_conf.h"

/*
 * USB buffer size of USB-CCID driver
 */
#if MAX_RES_APDU_DATA_SIZE > MAX_CMD_APDU_DATA_SIZE
#define USB_BUF_SIZE (MAX_RES_APDU_DATA_SIZE+5)
#else
#define USB_BUF_SIZE (MAX_CMD_APDU_DATA_SIZE+5)
#endif

struct apdu apdu;

/*
 * There are three layers in USB CCID implementation
 *
 *   +-------------------+
 *   | Application Layer |
 *   +-------------------+
 *      ^ command APDU |
 *      |              v response APDU
 *   +-------------------+
 *   |    CCID Layer     |
 *   +-------------------+
 *    ^ CCID PC_to_RDR  | CCID RDR_to_PC
 *    | Message         v Message
 *   +-------------------+
 *   |    USB Layer      |
 *   +-------------------+
 *    ^ USB             | USB
 *    | Bulk-OUT Packet v Bulk-IN Packet
 *
 */

/*
 * USB layer data structures
 */

struct ep_in {
  uint8_t ep_num;
  uint8_t tx_done;
  void (*notify) (struct ep_in *epi);
  const uint8_t *buf;
  size_t cnt;
  size_t buf_len;
  void *priv;
  void (*next_buf) (struct ep_in *epi, size_t len);
};

static void epi_init (struct ep_in *epi, int ep_num,
		      void (*notify) (struct ep_in *epi), void *priv)
{
  epi->ep_num = ep_num;
  epi->tx_done = 0;
  epi->notify = notify;
  epi->buf = NULL;
  epi->cnt = 0;
  epi->buf_len = 0;
  epi->priv = priv;
  epi->next_buf = NULL;
}

struct ep_out {
  uint8_t ep_num;
  uint8_t err;
  void (*notify) (struct ep_out *epo);
  uint8_t *buf;
  size_t cnt;
  size_t buf_len;
  void *priv;
  void (*next_buf) (struct ep_out *epo, size_t len);
  int  (*end_rx) (struct ep_out *epo, size_t orig_len);
};

static struct ep_out endpoint_out;
static struct ep_in endpoint_in;

static void epo_init (struct ep_out *epo, int ep_num,
		      void (*notify) (struct ep_out *epo), void *priv)
{
  epo->ep_num = ep_num;
  epo->err = 0;
  epo->notify = notify;
  epo->buf = NULL;
  epo->cnt = 0;
  epo->buf_len = 0;
  epo->priv = priv;
  epo->next_buf = NULL;
  epo->end_rx = NULL;
}

/*
 * CCID Layer
 */

/*
 * Buffer of USB communication: for both of RX and TX
 *
 * The buffer will be filled by multiple RX packets (Bulk-OUT)
 * or will be used for multiple TX packets (Bulk-IN)
 */
static uint8_t ccid_buffer[USB_BUF_SIZE];

#define CCID_SET_PARAMS		0x61 /* non-ICCD command  */
#define CCID_POWER_ON		0x62
#define CCID_POWER_OFF		0x63
#define CCID_SLOT_STATUS	0x65 /* non-ICCD command */
#define CCID_SECURE		0x69 /* non-ICCD command */
#define CCID_GET_PARAMS		0x6C /* non-ICCD command */
#define CCID_RESET_PARAMS	0x6D /* non-ICCD command */
#define CCID_XFR_BLOCK		0x6F
#define CCID_DATA_BLOCK_RET	0x80
#define CCID_SLOT_STATUS_RET	0x81 /* non-ICCD result */
#define CCID_PARAMS_RET		0x82 /* non-ICCD result */

#define CCID_MSG_SEQ_OFFSET	6
#define CCID_MSG_STATUS_OFFSET	7
#define CCID_MSG_ERROR_OFFSET	8
#define CCID_MSG_CHAIN_OFFSET	9
#define CCID_MSG_DATA_OFFSET	10	/* == CCID_MSG_HEADER_SIZE */
#define CCID_MAX_MSG_DATA_SIZE	USB_BUF_SIZE

#define CCID_STATUS_RUN		0x00
#define CCID_STATUS_PRESENT	0x01
#define CCID_STATUS_NOTPRESENT	0x02
#define CCID_CMD_STATUS_OK	0x00
#define CCID_CMD_STATUS_ERROR	0x40
#define CCID_CMD_STATUS_TIMEEXT	0x80

#define CCID_ERROR_XFR_OVERRUN	0xFC

/*
 * Since command-byte is at offset 0,
 * error with offset 0 means "command not supported".
 */
#define CCID_OFFSET_CMD_NOT_SUPPORTED 0
#define CCID_OFFSET_DATA_LEN 1
#define CCID_OFFSET_PARAM 8

struct ccid_header {
  uint8_t msg_type;
  uint32_t data_len;
  uint8_t slot;
  uint8_t seq;
  uint8_t rsvd;
  uint16_t param;
} __attribute__((packed));


/* Data structure handled by CCID layer */
struct ccid {
  enum ccid_state ccid_state;
  uint8_t state;
  uint8_t *p;
  size_t len;

  uint8_t err;

  struct ccid_header ccid_header;

  uint8_t sw1sw2[2];
  uint8_t chained_cls_ins_p1_p2[4];

  /* lower layer */
  struct ep_out *epo;
  struct ep_in *epi;

  /* from both layers */
  struct eventflag ccid_comm;

  /* upper layer */
  struct eventflag openpgp_comm;
  chopstx_t application;
  struct apdu *a;
};

/*
 * APDU_STATE_WAIT_COMMAND           +---------+
 *        |           |              |         |
 *        |           v              v         |
 *        |      APDU_STATE_COMMAND_CHAINING --+
 *        |                 |
 *        v                 v
 *      APDU_STATE_COMMAND_RECEIVED
 *                  |
 *                  v
 *          ===================
 *          | Process COMMAND |
 *          ===================
 *                  |
 *                  v
 *            +-----+----------+                  +---------+
 *            |                |                  |         |
 *            v                v                  v         |
 * APDU_STATE_RESULT <---- APDU_STATE_RESULT_GET_RESPONSE --+
 *         |
 *         |
 *         v
 * APDU_STATE_WAIT_COMMAND
 */

#define APDU_STATE_WAIT_COMMAND        0
#define APDU_STATE_COMMAND_CHAINING    1
#define APDU_STATE_COMMAND_RECEIVED    2
#define APDU_STATE_RESULT              3
#define APDU_STATE_RESULT_GET_RESPONSE 4

static void ccid_reset (struct ccid *c)
{
  c->err = 0;
  c->state = APDU_STATE_WAIT_COMMAND;
  c->p = c->a->cmd_apdu_data;
  c->len = MAX_CMD_APDU_DATA_SIZE;
  c->a->cmd_apdu_data_len = 0;
  c->a->expected_res_size = 0;
}

static void ccid_init (struct ccid *c, struct ep_in *epi, struct ep_out *epo,
		       struct apdu *a)
{
  ccid_state_p = &c->ccid_state;

  c->ccid_state = CCID_STATE_START;
  c->state = APDU_STATE_WAIT_COMMAND;
  c->p = a->cmd_apdu_data;
  c->len = MAX_CMD_APDU_DATA_SIZE;
  c->err = 0;
  memset (&c->ccid_header, 0, sizeof (struct ccid_header));
  c->sw1sw2[0] = 0x90;
  c->sw1sw2[1] = 0x00;
  c->application = 0;
  c->epi = epi;
  c->epo = epo;
  c->a = a;
}

/*
 * Application layer
 */

/*
 * USB-CCID communication could be considered "half duplex".
 *
 * While the device is sending something, there is no possibility for
 * the device to receive anything.
 *
 * While the device is receiving something, there is no possibility
 * for the device to send anything.
 *
 * Thus, the buffer can be shared for RX and TX.
 *
 * Exception: When we support ABORT of CCID, it is possible to receive
 * ABORT Class Specific Request to control pipe while we are
 * receiving/sending something at OUT/IN endpoint.
 *
 */

#define CMD_APDU_HEAD_SIZE 5

static void apdu_init (struct apdu *a)
{
  a->seq = 0;			/* will be set by lower layer */
  a->cmd_apdu_head = &ccid_buffer[0];
  a->cmd_apdu_data = &ccid_buffer[5];
  a->cmd_apdu_data_len = 0;	/* will be set by lower layer */
  a->expected_res_size = 0;	/* will be set by lower layer */

  a->sw = 0x9000;		     /* will be set by upper layer */
  a->res_apdu_data = &ccid_buffer[5]; /* will be set by upper layer */
  a->res_apdu_data_len = 0;	     /* will be set by upper layer */
}


static void notify_tx (struct ep_in *epi)
{
  struct ccid *c = (struct ccid *)epi->priv;

  /* The sequence of Bulk-IN transactions finished */
  eventflag_signal (&c->ccid_comm, EV_TX_FINISHED);
}

static void no_buf (struct ep_in *epi, size_t len)
{
  (void)len;
  epi->buf = NULL;
  epi->cnt = 0;
  epi->buf_len = 0;
}

static void set_sw1sw2 (struct ccid *c, size_t chunk_len)
{
  if (c->a->expected_res_size >= c->len)
    {
      c->sw1sw2[0] = 0x90;
      c->sw1sw2[1] = 0x00;
    }
  else
    {
      c->sw1sw2[0] = 0x61;
      if (c->len - chunk_len >= 256)
	c->sw1sw2[1] = 0;
      else
	c->sw1sw2[1] = (uint8_t)(c->len - chunk_len);
    }
}

static void get_sw1sw2 (struct ep_in *epi, size_t len)
{
  struct ccid *c = (struct ccid *)epi->priv;

  (void)len;
  epi->buf = c->sw1sw2;
  epi->cnt = 0;
  epi->buf_len = 2;
  epi->next_buf = no_buf;
}


/*
 * Tx done callback
 */
static void
EP1_IN_Callback (uint16_t len)
{
  struct ep_in *epi = &endpoint_in;

  (void)len;
  if (epi->buf == NULL)
    if (epi->tx_done)
      epi->notify (epi);
    else
      {
	epi->tx_done = 1;
	usb_lld_tx_enable (epi->ep_num, 0); /* send ZLP */
      }
  else
    {
      int tx_size = 0;
      size_t remain = USB_LL_BUF_SIZE;
      int offset = 0;

      while (epi->buf)
	if (epi->buf_len < remain)
	  {
	    usb_lld_txcpy (epi->buf, epi->ep_num, offset, epi->buf_len);
	    offset += epi->buf_len;
	    remain -= epi->buf_len;
	    tx_size += epi->buf_len;
	    epi->next_buf (epi, remain); /* Update epi->buf, cnt, buf_len */
	  }
	else
	  {
	    usb_lld_txcpy (epi->buf, epi->ep_num, offset, remain);
	    epi->buf += remain;
	    epi->cnt += remain;
	    epi->buf_len -= remain;
	    tx_size += remain;
	    break;
	  }

      if (tx_size < USB_LL_BUF_SIZE)
	epi->tx_done = 1;
      usb_lld_tx_enable (epi->ep_num, tx_size);
    }
}


static void notify_icc (struct ep_out *epo)
{
  struct ccid *c = (struct ccid *)epo->priv;

  c->err = epo->err;
  eventflag_signal (&c->ccid_comm, EV_RX_DATA_READY);
}

static int end_ccid_rx (struct ep_out *epo, size_t orig_len)
{
  (void)orig_len;
  if (epo->cnt < sizeof (struct ccid_header))
    /* short packet, just ignore */
    return 1;

  /* icc message with no abdata */
  return 0;
}

static int end_abdata (struct ep_out *epo, size_t orig_len)
{
  struct ccid *c = (struct ccid *)epo->priv;
  size_t len = epo->cnt;

  if (orig_len == USB_LL_BUF_SIZE && len < c->ccid_header.data_len)
    /* more packet comes */
    return 1;

  if (len != c->ccid_header.data_len)
    epo->err = 1;

  return 0;
}

static int end_cmd_apdu_head (struct ep_out *epo, size_t orig_len)
{
  struct ccid *c = (struct ccid *)epo->priv;

  (void)orig_len;

  if (epo->cnt < 4 || epo->cnt != c->ccid_header.data_len)
    {
      epo->err = 1;
      return 0;
    }

  if ((c->state == APDU_STATE_COMMAND_CHAINING)
      && (c->chained_cls_ins_p1_p2[0] != (c->a->cmd_apdu_head[0] & ~0x10)
	  || c->chained_cls_ins_p1_p2[1] != c->a->cmd_apdu_head[1]
	  || c->chained_cls_ins_p1_p2[2] != c->a->cmd_apdu_head[2]
	  || c->chained_cls_ins_p1_p2[3] != c->a->cmd_apdu_head[3]))
    /*
     * Handling exceptional request.
     *
     * Host stops sending command APDU using command chaining,
     * and start another command APDU.
     *
     * Discard old one, and start handling new one.
     */
    {
      c->state = APDU_STATE_WAIT_COMMAND;
      c->p = c->a->cmd_apdu_data;
      c->len = MAX_CMD_APDU_DATA_SIZE;
    }

  if (epo->cnt == 4)
    /* No Lc and Le */
    c->a->expected_res_size = 0;
  else if (epo->cnt == 5)
    {
      /* No Lc but Le */
      c->a->expected_res_size = c->a->cmd_apdu_head[4];
      if (c->a->expected_res_size == 0)
	c->a->expected_res_size = 256;
      c->a->cmd_apdu_head[4] = 0;
    }

  c->a->cmd_apdu_data_len = 0;
  return 0;
}


static int end_nomore_data (struct ep_out *epo, size_t orig_len)
{
  (void)epo;
  if (orig_len == USB_LL_BUF_SIZE)
    return 1;
  else
    return 0;
}


static int end_cmd_apdu_data (struct ep_out *epo, size_t orig_len)
{
  struct ccid *c = (struct ccid *)epo->priv;
  size_t len = epo->cnt;

  if (orig_len == USB_LL_BUF_SIZE
      && CMD_APDU_HEAD_SIZE + len < c->ccid_header.data_len)
    /* more packet comes */
    return 1;

  if (CMD_APDU_HEAD_SIZE + len != c->ccid_header.data_len)
    goto error;

  if (len == c->a->cmd_apdu_head[4])
    /* No Le field*/
    c->a->expected_res_size = 0;
  else if (len == (size_t)c->a->cmd_apdu_head[4] + 1)
    {
      /* it has Le field*/
      c->a->expected_res_size = epo->buf[-1];
      if (c->a->expected_res_size == 0)
	c->a->expected_res_size = 256;
      len--;
    }
  else
    {
    error:
      epo->err = 1;
      return 0;
    }

  c->a->cmd_apdu_data_len += len;
  return 0;
}


static void nomore_data (struct ep_out *epo, size_t len)
{
  (void)len;
  epo->err = 1;
  epo->end_rx = end_nomore_data;
  epo->buf = NULL;
  epo->buf_len = 0;
  epo->cnt = 0;
  epo->next_buf = nomore_data;
}

#define INS_GET_RESPONSE 0xc0

static void ccid_cmd_apdu_data (struct ep_out *epo, size_t len)
{
  struct ccid *c = (struct ccid *)epo->priv;

  (void)len;
  if (c->state == APDU_STATE_RESULT_GET_RESPONSE
      && c->a->cmd_apdu_head[1] != INS_GET_RESPONSE)
    {
      /*
       * Handling exceptional request.
       *
       * Host didn't finish receiving the whole response APDU by GET RESPONSE,
       * but initiates another command.
       */

      c->state = APDU_STATE_WAIT_COMMAND;
      c->p = c->a->cmd_apdu_data;
      c->len = MAX_CMD_APDU_DATA_SIZE;
    }
  else if (c->state == APDU_STATE_COMMAND_CHAINING)
    {
      if (c->chained_cls_ins_p1_p2[0] != (c->a->cmd_apdu_head[0] & ~0x10)
	  || c->chained_cls_ins_p1_p2[1] != c->a->cmd_apdu_head[1]
	  || c->chained_cls_ins_p1_p2[2] != c->a->cmd_apdu_head[2]
	  || c->chained_cls_ins_p1_p2[3] != c->a->cmd_apdu_head[3])
	/*
	 * Handling exceptional request.
	 *
	 * Host stops sending command APDU using command chaining,
	 * and start another command APDU.
	 *
	 * Discard old one, and start handling new one.
	 */
	{
	  c->state = APDU_STATE_WAIT_COMMAND;
	  c->p = c->a->cmd_apdu_data;
	  c->len = MAX_CMD_APDU_DATA_SIZE;
	  c->a->cmd_apdu_data_len = 0;
	}
    }

  epo->end_rx = end_cmd_apdu_data;
  epo->buf = c->p;
  epo->buf_len = c->len;
  epo->cnt = 0;
  epo->next_buf = nomore_data;
}

static void ccid_abdata (struct ep_out *epo, size_t len)
{
  struct ccid *c = (struct ccid *)epo->priv;

  (void)len;
  c->a->seq = c->ccid_header.seq;
  if (c->ccid_header.msg_type == CCID_XFR_BLOCK)
    {
      c->a->seq = c->ccid_header.seq;
      epo->end_rx = end_cmd_apdu_head;
      epo->buf = c->a->cmd_apdu_head;
      epo->buf_len = 5;
      epo->cnt = 0;
      epo->next_buf = ccid_cmd_apdu_data;
    }
  else
    {
      epo->end_rx = end_abdata;
      epo->buf = c->p;
      epo->buf_len = c->len;
      epo->cnt = 0;
      epo->next_buf = nomore_data;
    }
}


static void
ccid_prepare_receive (struct ccid *c)
{
  c->epo->err = 0;
  c->epo->buf = (uint8_t *)&c->ccid_header;
  c->epo->buf_len = sizeof (struct ccid_header);
  c->epo->cnt = 0;
  c->epo->next_buf = ccid_abdata;
  c->epo->end_rx = end_ccid_rx;
  usb_lld_rx_enable (c->epo->ep_num);
  DEBUG_INFO ("Rx ready\r\n");
}

/*
 * Rx ready callback
 */

static void
EP1_OUT_Callback (uint16_t len)
{
  struct ep_out *epo = &endpoint_out;
  int offset = 0;
  int cont;
  size_t orig_len = len;

  while (epo->err == 0)
    if (len == 0)
      break;
    else if (len <= epo->buf_len)
      {
	usb_lld_rxcpy (epo->buf, epo->ep_num, offset, len);
	epo->buf += len;
	epo->cnt += len;
	epo->buf_len -= len;
	break;
      }
    else /* len > buf_len */
      {
	usb_lld_rxcpy (epo->buf, epo->ep_num, offset, epo->buf_len);
	len -= epo->buf_len;
	offset += epo->buf_len;
	epo->next_buf (epo, len); /* Update epo->buf, cnt, buf_len */
      }

  /*
   * ORIG_LEN to distingush ZLP and the end of transaction
   *  (ORIG_LEN != USB_LL_BUF_SIZE)
   */
  cont = epo->end_rx (epo, orig_len);

  if (cont)
    usb_lld_rx_enable (epo->ep_num);
  else
    epo->notify (epo);
}


extern void EP6_IN_Callback (uint16_t len);

static void
usb_rx_ready (uint8_t ep_num, uint16_t len)
{
  if (ep_num == ENDP1)
    EP1_OUT_Callback (len);
#ifdef DEBUG
  else if (ep_num == ENDP5)
    {
      chopstx_mutex_lock (&stdout.m_dev);
      usb_lld_rx_enable (ep_num);
      chopstx_mutex_unlock (&stdout.m_dev);
    }
#endif
}

static void
usb_tx_done (uint8_t ep_num, uint16_t len)
{
  if (ep_num == ENDP1)
    EP1_IN_Callback (len);
  else if (ep_num == ENDP2)
    {
      /* INTERRUPT Transfer done */
    }
#ifdef DEBUG
  else if (ep_num == ENDP3)
    {
      chopstx_mutex_lock (&stdout.m_dev);
      chopstx_cond_signal (&stdout.cond_dev);
      chopstx_mutex_unlock (&stdout.m_dev);
    }
#endif
#ifdef PINPAD_SUPPORT
  else if (ep_num == ENDP6)
    EP6_IN_Callback (len);
#endif
}

/*
 * ATR (Answer To Reset) string
 *
 * TS = 0x3b: Direct conversion
 * T0 = 0xda: TA1, TC1 and TD1 follow, 10 historical bytes
 * TA1 = 0x11: FI=1, DI=1
 * TC1 = 0xff
 * TD1 = 0x81: TD2 follows, T=1
 * TD2 = 0xb1: TA3, TB3 and TD3 follow, T=1
 * TA3 = 0xFE: IFSC = 254 bytes
 * TB3 = 0x55: BWI = 5, CWI = 5   (BWT timeout 3.2 sec)
 * TD3 = 0x1f: TA4 follows, T=15
 * TA4 = 0x03: 5V or 3.3V
 * Historical bytes: to be explained...
 * XOR check
 *
 * Minimum: 0x3b, 0x8a, 0x80, 0x01, + historical bytes, xor check
 *
 */
static const uint8_t ATR[] = {
  0x3b, 0xda, 0x11, 0xff, 0x81, 0xb1, 0xfe, 0x55, 0x1f, 0x03,
  0x00,
	0x31, 0x84, /* full DF name, GET DATA, MF */
        0x73,
              0x80, /* DF full name */
  	      0x01, /* 1-byte */
  	      0x80, /* Command chaining, No extended Lc and extended Le */
  	0x00,
        0x90, 0x00,
 (0xda^0x11^0xff^0x81^0xb1^0xfe^0x55^0x1f^0x03
  ^0x00^0x31^0x84^0x73^0x80^0x01^0x80^0x00^0x90^0x00)
};

/*
 * Send back error
 */
static void ccid_error (struct ccid *c, int offset)
{
  uint8_t ccid_reply[CCID_MSG_HEADER_SIZE];

  ccid_reply[0] = CCID_SLOT_STATUS_RET; /* Any value should be OK */
  ccid_reply[1] = 0x00;
  ccid_reply[2] = 0x00;
  ccid_reply[3] = 0x00;
  ccid_reply[4] = 0x00;
  ccid_reply[5] = 0x00;	/* Slot */
  ccid_reply[CCID_MSG_SEQ_OFFSET] = c->ccid_header.seq;
  if (c->ccid_state == CCID_STATE_NOCARD)
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 2; /* 2: No ICC present */
  else if (c->ccid_state == CCID_STATE_START)
    /* 1: ICC present but not activated */
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 1;
  else
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 0; /* An ICC is present and active */
  ccid_reply[CCID_MSG_STATUS_OFFSET] |= CCID_CMD_STATUS_ERROR; /* Failed */
  ccid_reply[CCID_MSG_ERROR_OFFSET] = offset;
  ccid_reply[CCID_MSG_CHAIN_OFFSET] = 0x00;

  /* This is a single packet Bulk-IN transaction */
  c->epi->buf = NULL;
  c->epi->tx_done = 1;
  usb_lld_write (c->epi->ep_num, ccid_reply, CCID_MSG_HEADER_SIZE);
}

extern void *openpgp_card_thread (void *arg);

extern uint8_t __process3_stack_base__, __process3_stack_size__;
const uint32_t __stackaddr_gpg = (uint32_t)&__process3_stack_base__;
const size_t __stacksize_gpg = (size_t)&__process3_stack_size__;
#define PRIO_GPG 1


/* Send back ATR (Answer To Reset) */
static enum ccid_state
ccid_power_on (struct ccid *c)
{
  size_t size_atr = sizeof (ATR);
  uint8_t p[CCID_MSG_HEADER_SIZE];

  if (c->application == 0)
    c->application = chopstx_create (PRIO_GPG, __stackaddr_gpg,
				     __stacksize_gpg, openpgp_card_thread,
				     (void *)&c->ccid_comm);

  p[0] = CCID_DATA_BLOCK_RET;
  p[1] = size_atr;
  p[2] = 0x00;
  p[3] = 0x00;
  p[4] = 0x00;
  p[5] = 0x00;	/* Slot */
  p[CCID_MSG_SEQ_OFFSET] = c->ccid_header.seq;
  p[CCID_MSG_STATUS_OFFSET] = 0x00;
  p[CCID_MSG_ERROR_OFFSET] = 0x00;
  p[CCID_MSG_CHAIN_OFFSET] = 0x00;

  usb_lld_txcpy (p, c->epi->ep_num, 0, CCID_MSG_HEADER_SIZE);
  usb_lld_txcpy (ATR, c->epi->ep_num, CCID_MSG_HEADER_SIZE, size_atr);

  /* This is a single packet Bulk-IN transaction */
  c->epi->buf = NULL;
  c->epi->tx_done = 1;
  usb_lld_tx_enable (c->epi->ep_num, CCID_MSG_HEADER_SIZE + size_atr);
  DEBUG_INFO ("ON\r\n");

  return CCID_STATE_WAIT;
}

static void
ccid_send_status (struct ccid *c)
{
  uint8_t ccid_reply[CCID_MSG_HEADER_SIZE];

  ccid_reply[0] = CCID_SLOT_STATUS_RET;
  ccid_reply[1] = 0x00;
  ccid_reply[2] = 0x00;
  ccid_reply[3] = 0x00;
  ccid_reply[4] = 0x00;
  ccid_reply[5] = 0x00;	/* Slot */
  ccid_reply[CCID_MSG_SEQ_OFFSET] = c->ccid_header.seq;
  if (c->ccid_state == CCID_STATE_NOCARD)
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 2; /* 2: No ICC present */
  else if (c->ccid_state == CCID_STATE_START)
    /* 1: ICC present but not activated */
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 1;
  else
    ccid_reply[CCID_MSG_STATUS_OFFSET] = 0; /* An ICC is present and active */
  ccid_reply[CCID_MSG_ERROR_OFFSET] = 0x00;
  ccid_reply[CCID_MSG_CHAIN_OFFSET] = 0x00;

  /* This is a single packet Bulk-IN transaction */
  c->epi->buf = NULL;
  c->epi->tx_done = 1;
  usb_lld_write (c->epi->ep_num, ccid_reply, CCID_MSG_HEADER_SIZE);

  led_blink (LED_SHOW_STATUS);
#ifdef DEBUG_MORE
  DEBUG_INFO ("St\r\n");
#endif
}

static enum ccid_state
ccid_power_off (struct ccid *c)
{
  if (c->application)
    {
      eventflag_signal (&c->openpgp_comm, EV_EXIT);
      chopstx_join (c->application, NULL);
      c->application = 0;
    }

  c->ccid_state = CCID_STATE_START; /* This status change should be here */
  ccid_send_status (c);
  DEBUG_INFO ("OFF\r\n");
  return CCID_STATE_START;
}

static void
ccid_send_data_block_internal (struct ccid *c, uint8_t status, uint8_t error)
{
  int tx_size = USB_LL_BUF_SIZE;
  uint8_t p[CCID_MSG_HEADER_SIZE];
  size_t len;

  if (status == 0)
    len = c->a->res_apdu_data_len + 2;
  else
    len = 0;

  p[0] = CCID_DATA_BLOCK_RET;
  p[1] = len & 0xFF;
  p[2] = (len >> 8)& 0xFF;
  p[3] = (len >> 16)& 0xFF;
  p[4] = (len >> 24)& 0xFF;
  p[5] = 0x00;	/* Slot */
  p[CCID_MSG_SEQ_OFFSET] = c->a->seq;
  p[CCID_MSG_STATUS_OFFSET] = status;
  p[CCID_MSG_ERROR_OFFSET] = error;
  p[CCID_MSG_CHAIN_OFFSET] = 0;

  usb_lld_txcpy (p, c->epi->ep_num, 0, CCID_MSG_HEADER_SIZE);
  if (len == 0)
    {
      usb_lld_tx_enable (c->epi->ep_num, CCID_MSG_HEADER_SIZE);
      return;
    }

  if (CCID_MSG_HEADER_SIZE + len <= USB_LL_BUF_SIZE)
    {
      usb_lld_txcpy (c->a->res_apdu_data, c->epi->ep_num,
		     CCID_MSG_HEADER_SIZE, c->a->res_apdu_data_len);
      usb_lld_txcpy (c->sw1sw2, c->epi->ep_num,
		     CCID_MSG_HEADER_SIZE + c->a->res_apdu_data_len, 2);
      c->epi->buf = NULL;
      if (CCID_MSG_HEADER_SIZE + len < USB_LL_BUF_SIZE)
	c->epi->tx_done = 1;
      tx_size = CCID_MSG_HEADER_SIZE + len;
    }
  else if (CCID_MSG_HEADER_SIZE + len - 1 == USB_LL_BUF_SIZE)
    {
      usb_lld_txcpy (c->a->res_apdu_data, c->epi->ep_num,
		     CCID_MSG_HEADER_SIZE, c->a->res_apdu_data_len);
      usb_lld_txcpy (c->sw1sw2, c->epi->ep_num,
		     CCID_MSG_HEADER_SIZE + c->a->res_apdu_data_len, 1);
      c->epi->buf = &c->sw1sw2[1];
      c->epi->cnt = 1;
      c->epi->buf_len = 1;
      c->epi->next_buf = no_buf;
    }
  else if (CCID_MSG_HEADER_SIZE + len - 2 == USB_LL_BUF_SIZE)
    {
      usb_lld_txcpy (c->a->res_apdu_data, c->epi->ep_num,
		     CCID_MSG_HEADER_SIZE, c->a->res_apdu_data_len);
      c->epi->buf = &c->sw1sw2[0];
      c->epi->cnt = 0;
      c->epi->buf_len = 2;
      c->epi->next_buf = no_buf;
    }
  else
    {
      usb_lld_txcpy (c->a->res_apdu_data, c->epi->ep_num, CCID_MSG_HEADER_SIZE,
		     USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE);
      c->epi->buf = c->a->res_apdu_data + USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE;
      c->epi->cnt = USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE;
      c->epi->buf_len = c->a->res_apdu_data_len
	- (USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE);
      c->epi->next_buf = get_sw1sw2;
    }

  usb_lld_tx_enable (c->epi->ep_num, tx_size);
#ifdef DEBUG_MORE
  DEBUG_INFO ("DATA\r\n");
#endif
}

static void
ccid_send_data_block (struct ccid *c)
{
  ccid_send_data_block_internal (c, 0, 0);
}

static void
ccid_send_data_block_time_extension (struct ccid *c)
{
  ccid_send_data_block_internal (c, CCID_CMD_STATUS_TIMEEXT, 1);
}

static void
ccid_send_data_block_0x9000 (struct ccid *c)
{
  uint8_t p[CCID_MSG_HEADER_SIZE+2];
  size_t len = 2;

  p[0] = CCID_DATA_BLOCK_RET;
  p[1] = len & 0xFF;
  p[2] = (len >> 8)& 0xFF;
  p[3] = (len >> 16)& 0xFF;
  p[4] = (len >> 24)& 0xFF;
  p[5] = 0x00;	/* Slot */
  p[CCID_MSG_SEQ_OFFSET] = c->a->seq;
  p[CCID_MSG_STATUS_OFFSET] = 0;
  p[CCID_MSG_ERROR_OFFSET] = 0;
  p[CCID_MSG_CHAIN_OFFSET] = 0;
  p[CCID_MSG_CHAIN_OFFSET+1] = 0x90;
  p[CCID_MSG_CHAIN_OFFSET+2] = 0x00;

  usb_lld_txcpy (p, c->epi->ep_num, 0, CCID_MSG_HEADER_SIZE + len);
  c->epi->buf = NULL;
  c->epi->tx_done = 1;

  usb_lld_tx_enable (c->epi->ep_num, CCID_MSG_HEADER_SIZE + len);
#ifdef DEBUG_MORE
  DEBUG_INFO ("DATA\r\n");
#endif
}

/*
 * Reply to the host for "GET RESPONSE".
 */
static void
ccid_send_data_block_gr (struct ccid *c, size_t chunk_len)
{
  int tx_size = USB_LL_BUF_SIZE;
  uint8_t p[CCID_MSG_HEADER_SIZE];
  size_t len = chunk_len + 2;

  p[0] = CCID_DATA_BLOCK_RET;
  p[1] = len & 0xFF;
  p[2] = (len >> 8)& 0xFF;
  p[3] = (len >> 16)& 0xFF;
  p[4] = (len >> 24)& 0xFF;
  p[5] = 0x00;	/* Slot */
  p[CCID_MSG_SEQ_OFFSET] = c->a->seq;
  p[CCID_MSG_STATUS_OFFSET] = 0;
  p[CCID_MSG_ERROR_OFFSET] = 0;
  p[CCID_MSG_CHAIN_OFFSET] = 0;

  usb_lld_txcpy (p, c->epi->ep_num, 0, CCID_MSG_HEADER_SIZE);

  set_sw1sw2 (c, chunk_len);

  if (chunk_len <= USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE)
    {
      int size_for_sw;

      if (chunk_len <= USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE - 2)
	size_for_sw = 2;
      else if (chunk_len == USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE - 1)
	size_for_sw = 1;
      else
	size_for_sw = 0;

      usb_lld_txcpy (c->p, c->epi->ep_num, CCID_MSG_HEADER_SIZE, chunk_len);
      if (size_for_sw)
	usb_lld_txcpy (c->sw1sw2, c->epi->ep_num,
		       CCID_MSG_HEADER_SIZE + chunk_len, size_for_sw);
      tx_size = CCID_MSG_HEADER_SIZE + chunk_len + size_for_sw;
      if (size_for_sw == 2)
	{
	  c->epi->buf = NULL;
	  if (tx_size < USB_LL_BUF_SIZE)
	    c->epi->tx_done = 1;
	  /* Don't set epi->tx_done = 1, when it requires ZLP */
	}
      else
	{
	  c->epi->buf = c->sw1sw2 + size_for_sw;
	  c->epi->cnt = size_for_sw;
	  c->epi->buf_len = 2 - size_for_sw;
	  c->epi->next_buf = no_buf;
	}
    }
  else
    {
      usb_lld_txcpy (c->p, c->epi->ep_num, CCID_MSG_HEADER_SIZE,
		     USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE);
      c->epi->buf = c->p + USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE;
      c->epi->cnt = 0;
      c->epi->buf_len = chunk_len - (USB_LL_BUF_SIZE - CCID_MSG_HEADER_SIZE);
      c->epi->next_buf = get_sw1sw2;
    }

  c->p += chunk_len;
  c->len -= chunk_len;
  usb_lld_tx_enable (c->epi->ep_num, tx_size);
#ifdef DEBUG_MORE
  DEBUG_INFO ("DATA\r\n");
#endif
}


static void
ccid_send_params (struct ccid *c)
{
  uint8_t p[CCID_MSG_HEADER_SIZE];
  const uint8_t params[] =  {
    0x11,   /* bmFindexDindex */
    0x11, /* bmTCCKST1 */
    0xFE, /* bGuardTimeT1 */
    0x55, /* bmWaitingIntegersT1 */
    0x03, /* bClockStop */
    0xFE, /* bIFSC */
    0    /* bNadValue */
  };

  p[0] = CCID_PARAMS_RET;
  p[1] = 0x07;	/* Length = 0x00000007 */
  p[2] = 0;
  p[3] = 0;
  p[4] = 0;
  p[5] = 0x00;	/* Slot */
  p[CCID_MSG_SEQ_OFFSET] = c->ccid_header.seq;
  p[CCID_MSG_STATUS_OFFSET] = 0;
  p[CCID_MSG_ERROR_OFFSET] = 0;
  p[CCID_MSG_CHAIN_OFFSET] = 0x01;  /* ProtocolNum: T=1 */

  usb_lld_txcpy (p, c->epi->ep_num, 0, CCID_MSG_HEADER_SIZE);
  usb_lld_txcpy (params, c->epi->ep_num, CCID_MSG_HEADER_SIZE, sizeof params);

  /* This is a single packet Bulk-IN transaction */
  c->epi->buf = NULL;
  c->epi->tx_done = 1;
  usb_lld_tx_enable (c->epi->ep_num, CCID_MSG_HEADER_SIZE + sizeof params);
#ifdef DEBUG_MORE
  DEBUG_INFO ("PARAMS\r\n");
#endif
}


static enum ccid_state
ccid_handle_data (struct ccid *c)
{
  enum ccid_state next_state = c->ccid_state;

  if (c->err != 0)
    {
      ccid_reset (c);
      ccid_error (c, CCID_OFFSET_DATA_LEN);
      return next_state;
    }

  switch (c->ccid_state)
    {
    case CCID_STATE_NOCARD:
      if (c->ccid_header.msg_type == CCID_SLOT_STATUS)
	ccid_send_status (c);
      else
	{
	  DEBUG_INFO ("ERR00\r\n");
	  ccid_error (c, CCID_OFFSET_CMD_NOT_SUPPORTED);
	}
      break;
    case CCID_STATE_START:
      if (c->ccid_header.msg_type == CCID_POWER_ON)
	{
	  ccid_reset (c);
	  next_state = ccid_power_on (c);
	}
      else if (c->ccid_header.msg_type == CCID_POWER_OFF)
	{
	  ccid_reset (c);
	  next_state = ccid_power_off (c);
	}
      else if (c->ccid_header.msg_type == CCID_SLOT_STATUS)
	ccid_send_status (c);
      else
	{
	  DEBUG_INFO ("ERR01\r\n");
	  ccid_error (c, CCID_OFFSET_CMD_NOT_SUPPORTED);
	}
      break;
    case CCID_STATE_WAIT:
      if (c->ccid_header.msg_type == CCID_POWER_ON)
	{
	  /* Not in the spec., but pcscd/libccid */
	  ccid_reset (c);
	  next_state = ccid_power_on (c);
	}
      else if (c->ccid_header.msg_type == CCID_POWER_OFF)
	{
	  ccid_reset (c);
	  next_state = ccid_power_off (c);
	}
      else if (c->ccid_header.msg_type == CCID_SLOT_STATUS)
	ccid_send_status (c);
      else if (c->ccid_header.msg_type == CCID_XFR_BLOCK)
	{
	  if (c->ccid_header.param == 0)
	    {
	      if ((c->a->cmd_apdu_head[0] & 0x10) == 0)
		{
		  if (c->state == APDU_STATE_COMMAND_CHAINING)
		    {		/* command chaining finished */
		      c->p += c->a->cmd_apdu_head[4];
		      c->a->cmd_apdu_head[4] = 0;
		      DEBUG_INFO ("CMD chaning finished.\r\n");
		    }

		  if (c->a->cmd_apdu_head[1] == INS_GET_RESPONSE
		      && c->state == APDU_STATE_RESULT_GET_RESPONSE)
		    {
		      size_t len = c->a->expected_res_size;

		      if (c->len <= c->a->expected_res_size)
			len = c->len;

		      ccid_send_data_block_gr (c, len);
		      if (c->len == 0)
			c->state = APDU_STATE_RESULT;
		      c->ccid_state = CCID_STATE_WAIT;
		      DEBUG_INFO ("GET Response.\r\n");
		    }
		  else
		    {		  /* Give this message to GPG thread */
		      c->state = APDU_STATE_COMMAND_RECEIVED;

		      c->a->sw = 0x9000;
		      c->a->res_apdu_data_len = 0;
		      c->a->res_apdu_data = &ccid_buffer[5];

		      eventflag_signal (&c->openpgp_comm, EV_CMD_AVAILABLE);
		      next_state = CCID_STATE_EXECUTE;
		    }
		}
	      else
		{
		  if (c->state == APDU_STATE_WAIT_COMMAND)
		    {		/* command chaining is started */
		      c->a->cmd_apdu_head[0] &= ~0x10;
		      memcpy (c->chained_cls_ins_p1_p2, c->a->cmd_apdu_head, 4);
		      c->state = APDU_STATE_COMMAND_CHAINING;
		    }

		  c->p += c->a->cmd_apdu_head[4];
		  c->len -= c->a->cmd_apdu_head[4];
		  ccid_send_data_block_0x9000 (c);
		  DEBUG_INFO ("CMD chaning...\r\n");
		}
	    }
	  else
	    {		     /* ICC block chaining is not supported. */
	      DEBUG_INFO ("ERR02\r\n");
	      ccid_error (c, CCID_OFFSET_PARAM);
	    }
	}
      else if (c->ccid_header.msg_type == CCID_SET_PARAMS
	       || c->ccid_header.msg_type == CCID_GET_PARAMS
	       || c->ccid_header.msg_type == CCID_RESET_PARAMS)
	ccid_send_params (c);
      else if (c->ccid_header.msg_type == CCID_SECURE)
	{
	  if (c->p != c->a->cmd_apdu_data)
	    {
	      /* SECURE received in the middle of command chaining */
	      ccid_reset (c);
	      ccid_error (c, CCID_OFFSET_DATA_LEN);
	      return next_state;
	    }

	  if (c->p[10-10] == 0x00) /* PIN verification */
	    {
	      c->a->cmd_apdu_head[0] = c->p[25-10];
	      c->a->cmd_apdu_head[1] = c->p[26-10];
	      c->a->cmd_apdu_head[2] = c->p[27-10];
	      c->a->cmd_apdu_head[3] = c->p[28-10];
	      /**/
	      c->a->cmd_apdu_data[0] = 0; /* bConfirmPIN */
	      c->a->cmd_apdu_data[1] = c->p[17-10]; /* bEntryValidationCondition */
	      c->a->cmd_apdu_data[2] = c->p[18-10]; /* bNumberMessage */
	      c->a->cmd_apdu_data[3] = c->p[19-10]; /* wLangId L */
	      c->a->cmd_apdu_data[4] = c->p[20-10]; /* wLangId H */
	      c->a->cmd_apdu_data[5] = c->p[21-10]; /* bMsgIndex */

	      c->a->cmd_apdu_data_len = 6;
	      c->a->expected_res_size = 0;

	      c->a->sw = 0x9000;
	      c->a->res_apdu_data_len = 0;
	      c->a->res_apdu_data = &c->p[5];

	      eventflag_signal (&c->openpgp_comm, EV_VERIFY_CMD_AVAILABLE);
	      next_state = CCID_STATE_EXECUTE;
	    }
	  else if (c->p[10-10] == 0x01) /* PIN Modification */
	    {
	      uint8_t num_msgs = c->p[21-10];

	      if (num_msgs == 0x00)
		num_msgs = 1;
	      else if (num_msgs == 0xff)
		num_msgs = 3;
	      c->a->cmd_apdu_head[0] = c->p[27 + num_msgs-10];
	      c->a->cmd_apdu_head[1] = c->p[28 + num_msgs-10];
	      c->a->cmd_apdu_head[2] = c->p[29 + num_msgs-10];
	      c->a->cmd_apdu_head[3] = c->p[30 + num_msgs-10];
	      /**/
	      c->a->cmd_apdu_data[0] = c->p[19-10]; /* bConfirmPIN */
	      c->a->cmd_apdu_data[1] = c->p[20-10]; /* bEntryValidationCondition */
	      c->a->cmd_apdu_data[2] = c->p[21-10]; /* bNumberMessage */
	      c->a->cmd_apdu_data[3] = c->p[22-10]; /* wLangId L */
	      c->a->cmd_apdu_data[4] = c->p[23-10]; /* wLangId H */
	      c->a->cmd_apdu_data[5] = c->p[24-10]; /* bMsgIndex, bMsgIndex1 */
	      if (num_msgs >= 2)
		c->a->cmd_apdu_data[6] = c->p[25-10]; /* bMsgIndex2 */
	      if (num_msgs == 3)
		c->a->cmd_apdu_data[7] = c->p[26-10]; /* bMsgIndex3 */

	      c->a->cmd_apdu_data_len = 5 + num_msgs;
	      c->a->expected_res_size = 0;

	      c->a->sw = 0x9000;
	      c->a->res_apdu_data_len = 0;
	      c->a->res_apdu_data = &ccid_buffer[5];

	      eventflag_signal (&c->openpgp_comm, EV_MODIFY_CMD_AVAILABLE);
	      next_state = CCID_STATE_EXECUTE;
	    }
	  else
	    ccid_error (c, CCID_MSG_DATA_OFFSET);
	}
      else
	{
	  DEBUG_INFO ("ERR03\r\n");
	  DEBUG_BYTE (c->ccid_header.msg_type);
	  ccid_error (c, CCID_OFFSET_CMD_NOT_SUPPORTED);
	}
      break;
    case CCID_STATE_EXECUTE:
      if (c->ccid_header.msg_type == CCID_POWER_OFF)
	next_state = ccid_power_off (c);
      else if (c->ccid_header.msg_type == CCID_SLOT_STATUS)
	ccid_send_status (c);
      else
	{
	  DEBUG_INFO ("ERR04\r\n");
	  DEBUG_BYTE (c->ccid_header.msg_type);
	  ccid_error (c, CCID_OFFSET_CMD_NOT_SUPPORTED);
	}
      break;
    default:
      next_state = CCID_STATE_START;
      DEBUG_INFO ("ERR10\r\n");
      break;
    }

  return next_state;
}

static enum ccid_state
ccid_handle_timeout (struct ccid *c)
{
  enum ccid_state next_state = c->ccid_state;

  switch (c->ccid_state)
    {
    case CCID_STATE_EXECUTE:
      ccid_send_data_block_time_extension (c);
      break;
    default:
      break;
    }

  led_blink (LED_ONESHOT);
  return next_state;
}

static struct ccid ccid;
enum ccid_state *ccid_state_p = &ccid.ccid_state;

void
ccid_card_change_signal (int how)
{
  struct ccid *c = &ccid;

  if (how == CARD_CHANGE_TOGGLE
      || (c->ccid_state == CCID_STATE_NOCARD && how == CARD_CHANGE_INSERT)
      || (c->ccid_state != CCID_STATE_NOCARD && how == CARD_CHANGE_REMOVE))
    eventflag_signal (&c->ccid_comm, EV_CARD_CHANGE);
}

void
ccid_usb_reset (int full)
{
  struct ccid *c = &ccid;

  eventflag_signal (&c->ccid_comm,
		    full ? EV_USB_DEVICE_RESET : EV_USB_SET_INTERFACE);
}


#define NOTIFY_SLOT_CHANGE 0x50
static void
ccid_notify_slot_change (struct ccid *c)
{
  uint8_t msg;
  uint8_t notification[2];

  if (c->ccid_state == CCID_STATE_NOCARD)
    msg = 0x02;
  else
    msg = 0x03;

  notification[0] = NOTIFY_SLOT_CHANGE;
  notification[1] = msg;
  usb_lld_write (ENDP2, notification, sizeof notification);
  led_blink (LED_TWOSHOTS);
}


#define USB_CCID_TIMEOUT (1950*1000)

#define GPG_THREAD_TERMINATED 0xffff


#define INTR_REQ_USB 20

extern uint32_t bDeviceState;
extern void usb_device_reset (struct usb_dev *dev);
extern int usb_setup (struct usb_dev *dev);
extern void usb_ctrl_write_finish (struct usb_dev *dev);
extern int usb_set_configuration (struct usb_dev *dev);
extern int usb_set_interface (struct usb_dev *dev);
extern int usb_get_interface (struct usb_dev *dev);
extern int usb_get_status_interface (struct usb_dev *dev);

extern int usb_get_descriptor (struct usb_dev *dev);

static void
usb_event_handle (struct usb_dev *dev)
{
  uint8_t ep_num;
  int e;

  e = usb_lld_event_handler (dev);
  ep_num = USB_EVENT_ENDP (e);

  if (ep_num != 0)
    {
      if (USB_EVENT_TXRX (e))
	usb_tx_done (ep_num, USB_EVENT_LEN (e));
      else
	usb_rx_ready (ep_num, USB_EVENT_LEN (e));
    }
  else
    switch (USB_EVENT_ID (e))
      {
      case USB_EVENT_DEVICE_RESET:
	usb_device_reset (dev);
	break;

      case USB_EVENT_DEVICE_ADDRESSED:
	bDeviceState = ADDRESSED;
	break;

      case USB_EVENT_GET_DESCRIPTOR:
	if (usb_get_descriptor (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_SET_CONFIGURATION:
	if (usb_set_configuration (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_SET_INTERFACE:
	if (usb_set_interface (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_CTRL_REQUEST:
	/* Device specific device request.  */
	if (usb_setup (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_GET_STATUS_INTERFACE:
	if (usb_get_status_interface (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_GET_INTERFACE:
	if (usb_get_interface (dev) < 0)
	  usb_lld_ctrl_error (dev);
	break;

      case USB_EVENT_SET_FEATURE_DEVICE:
      case USB_EVENT_SET_FEATURE_ENDPOINT:
      case USB_EVENT_CLEAR_FEATURE_DEVICE:
      case USB_EVENT_CLEAR_FEATURE_ENDPOINT:
	usb_lld_ctrl_ack (dev);
	break;

      case USB_EVENT_CTRL_WRITE_FINISH:
	/* Control WRITE transfer finished.  */
	usb_ctrl_write_finish (dev);
	break;

      case USB_EVENT_OK:
      case USB_EVENT_DEVICE_SUSPEND:
      default:
	break;
      }
}

static void
poll_event_intr (uint32_t *timeout, struct eventflag *ev, chopstx_intr_t *intr)
{
  chopstx_poll_cond_t poll_desc;
  struct chx_poll_head *pd_array[2] = {
    (struct chx_poll_head *)intr,
    (struct chx_poll_head *)&poll_desc
  };

  eventflag_prepare_poll (ev, &poll_desc);
  chopstx_poll (timeout, 2, pd_array);
}

void *
ccid_thread (void *arg)
{
  chopstx_intr_t interrupt;
  uint32_t timeout;
  struct usb_dev dev;
  struct ccid *c = &ccid;

  (void)arg;

  eventflag_init (&ccid.ccid_comm);
  eventflag_init (&ccid.openpgp_comm);

  usb_lld_init (&dev, USB_INITIAL_FEATURE);
  chopstx_claim_irq (&interrupt, INTR_REQ_USB);
  usb_event_handle (&dev);	/* For old SYS < 3.0 */

 device_reset:
  {
    struct ep_in *epi = &endpoint_in;
    struct ep_out *epo = &endpoint_out;
    struct apdu *a = &apdu;

    epi_init (epi, ENDP1, notify_tx, c);
    epo_init (epo, ENDP1, notify_icc, c);
    apdu_init (a);
    ccid_init (c, epi, epo, a);
  }

  while (bDeviceState != CONFIGURED)
    {
      poll_event_intr (NULL, &c->ccid_comm, &interrupt);
      if (interrupt.ready)
	usb_event_handle (&dev);

      eventflag_get (&c->ccid_comm);
      /* Ignore event while not-configured.  */
    }

 interface_reset:
  timeout = USB_CCID_TIMEOUT;
  ccid_prepare_receive (c);
  ccid_notify_slot_change (c);
  while (1)
    {
      eventmask_t m;

      poll_event_intr (&timeout, &c->ccid_comm, &interrupt);
      if (interrupt.ready)
	{
	  usb_event_handle (&dev);
	  continue;
	}

      timeout = USB_CCID_TIMEOUT;
      m = eventflag_get (&c->ccid_comm);

      if (m == EV_USB_DEVICE_RESET)
	{
	  if (c->application)
	    {
	      chopstx_cancel (c->application);
	      chopstx_join (c->application, NULL);
	      c->application = 0;
	    }
	  goto device_reset;
	}
      else if (m == EV_USB_SET_INTERFACE)
	/* Upon receival of SET_INTERFACE, the endpoint is reset to RX_NAK.
	 * Thus, we need to prepare receive again.
	 */
	goto interface_reset;
      else if (m == EV_CARD_CHANGE)
	{
	  if (c->ccid_state == CCID_STATE_NOCARD)
	    /* Inserted!  */
	    c->ccid_state = CCID_STATE_START;
	  else
	    { /* Removed!  */
	      if (c->application)
		{
		  eventflag_signal (&c->openpgp_comm, EV_EXIT);
		  chopstx_join (c->application, NULL);
		  c->application = 0;
		}

	      c->ccid_state = CCID_STATE_NOCARD;
	    }

	  ccid_notify_slot_change (c);
	}
      else if (m == EV_RX_DATA_READY)
	c->ccid_state = ccid_handle_data (c);
      else if (m == EV_EXEC_FINISHED)
	if (c->ccid_state == CCID_STATE_EXECUTE)
	  {
	    if (c->a->sw == GPG_THREAD_TERMINATED)
	      {
		c->sw1sw2[0] = 0x90;
		c->sw1sw2[1] = 0x00;
		c->state = APDU_STATE_RESULT;
		ccid_send_data_block (c);
		c->ccid_state = CCID_STATE_EXITED;
		break;
	      }

	    c->a->cmd_apdu_data_len = 0;
	    c->sw1sw2[0] = c->a->sw >> 8;
	    c->sw1sw2[1] = c->a->sw & 0xff;

	    if (c->a->res_apdu_data_len <= c->a->expected_res_size)
	      {
		c->state = APDU_STATE_RESULT;
		ccid_send_data_block (c);
		c->ccid_state = CCID_STATE_WAIT;
	      }
	    else
	      {
		c->state = APDU_STATE_RESULT_GET_RESPONSE;
		c->p = c->a->res_apdu_data;
		c->len = c->a->res_apdu_data_len;
		ccid_send_data_block_gr (c, c->a->expected_res_size);
		c->ccid_state = CCID_STATE_WAIT;
	      }
	  }
	else
	  {
	    DEBUG_INFO ("ERR07\r\n");
	  }
      else if (m == EV_TX_FINISHED)
	{
	  if (c->state == APDU_STATE_RESULT)
	    {
	      c->state = APDU_STATE_WAIT_COMMAND;
	      c->p = c->a->cmd_apdu_data;
	      c->len = MAX_CMD_APDU_DATA_SIZE;
	      c->err = 0;
	      c->a->cmd_apdu_data_len = 0;
	      c->a->expected_res_size = 0;
	    }

	  if (c->state == APDU_STATE_WAIT_COMMAND
	      || c->state == APDU_STATE_COMMAND_CHAINING
	      || c->state == APDU_STATE_RESULT_GET_RESPONSE)
	    ccid_prepare_receive (c);
	}
      else			/* Timeout */
	c->ccid_state = ccid_handle_timeout (c);
    }

  if (c->application)
    {
      chopstx_join (c->application, NULL);
      c->application = 0;
    }

  /* Loading reGNUal.  */
  while (bDeviceState != UNCONNECTED)
    {
      chopstx_intr_wait (&interrupt);
      usb_event_handle (&dev);
    }

  return NULL;
}


#ifdef DEBUG
static void
stdout_init (void)
{
  chopstx_mutex_init (&stdout.m);
  chopstx_mutex_init (&stdout.m_dev);
  chopstx_cond_init (&stdout.cond_dev);
  stdout.connected = 0;
}

void
_write (const char *s, int len)
{
  int packet_len;

  if (len == 0)
    return;

  chopstx_mutex_lock (&stdout.m);

  chopstx_mutex_lock (&stdout.m_dev);
  if (!stdout.connected)
    chopstx_cond_wait (&stdout.cond_dev, &stdout.m_dev);
  chopstx_mutex_unlock (&stdout.m_dev);

  do
    {
      packet_len =
	(len < VIRTUAL_COM_PORT_DATA_SIZE) ? len : VIRTUAL_COM_PORT_DATA_SIZE;

      chopstx_mutex_lock (&stdout.m_dev);
      usb_lld_write (ENDP3, s, packet_len);
      chopstx_cond_wait (&stdout.cond_dev, &stdout.m_dev);
      chopstx_mutex_unlock (&stdout.m_dev);

      s += packet_len;
      len -= packet_len;
    }
  /* Send a Zero-Length-Packet if the last packet is full size.  */
  while (len != 0 || packet_len == VIRTUAL_COM_PORT_DATA_SIZE);

  chopstx_mutex_unlock (&stdout.m);
}

#else
void
_write (const char *s, int size)
{
  (void)s;
  (void)size;
}
#endif
