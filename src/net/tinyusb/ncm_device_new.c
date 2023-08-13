/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2020 Jacob Berg Potter
 * Copyright (c) 2020 Peter Lawrence
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

/**
 * Some explanations
 * -----------------
 * - \a rhport:       is the USB port of the device, in most cases "0"
 * - \a itf_data_alt: if != 0 -> data xmt/rcv are allowed
 *
 * Glossary
 * --------
 * NTB - NCM Transfer Block
 */


#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "tusb_option.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"

#include "net_device.h"
#include "ncm.h"


#if !defined(tu_static)  ||  ECLIPSE_GUI
    // TinyUSB <=0.15.0 does not know "tu_static"
    #define tu_static static
#endif

//-----------------------------------------------------------------------------
//
// Module global things
//
typedef struct {
    uint8_t      ep_in;                         //!< endpoint for outgoing datagrams (naming is a little bit confusing)
    uint8_t      ep_out;                        //!< endpoint for incoming datagrams (naming is a little bit confusing)
    uint8_t      ep_notif;                      //!< endpoint for notifications
    uint8_t      itf_num;                       //!< interface number
    uint8_t      itf_data_alt;                  //!< ==0 -> no endpoints, i.e. no network traffic, ==1 -> normal operation with two endpoints (spec, chapter 5.3)
    uint8_t      rhport;                        //!< storage of \a rhport because some callbacks are done without it

    enum {
        NOTIFICATION_SPEED, NOTIFICATION_CONNECTED, NOTIFICATION_DONE
    } notification_xmt_state;
    bool         notification_xmt_is_running;   //!< notification is currently transmitted
} ncm_interface_t;


static ncm_interface_t ncm_interface;


/**
 * This is the NTB parameter structure
 *
 * \attention
 *     We are lucky, that byte order is correct
 */
CFG_TUSB_MEM_SECTION CFG_TUSB_MEM_ALIGN tu_static const ntb_parameters_t ntb_parameters = {
        .wLength                 = sizeof(ntb_parameters_t),
        .bmNtbFormatsSupported   = 0x01,                                 // 16-bit NTB supported
        .dwNtbInMaxSize          = CFG_TUD_NCM_IN_NTB_MAX_SIZE,
        .wNdbInDivisor           = 4,
        .wNdbInPayloadRemainder  = 0,
        .wNdbInAlignment         = CFG_TUD_NCM_ALIGNMENT,
        .wReserved               = 0,
        .dwNtbOutMaxSize         = CFG_TUD_NCM_OUT_NTB_MAX_SIZE,
        .wNdbOutDivisor          = 4,
        .wNdbOutPayloadRemainder = 0,
        .wNdbOutAlignment        = CFG_TUD_NCM_ALIGNMENT,
        .wNtbOutMaxDatagrams     = 0                                     // 0=no limit
};


//-----------------------------------------------------------------------------
//
// everything about notifications
//
tu_static struct ncm_notify_t ncm_notify_connected = {
        .header = {
                .bmRequestType_bit = {
                        .recipient = TUSB_REQ_RCPT_INTERFACE,
                        .type      = TUSB_REQ_TYPE_CLASS,
                        .direction = TUSB_DIR_IN
                },
                .bRequest = CDC_NOTIF_NETWORK_CONNECTION,
                .wValue   = 1 /* Connected */,
                .wLength  = 0,
        },
};

tu_static struct ncm_notify_t ncm_notify_speed_change = {
        .header = {
                .bmRequestType_bit = {
                        .recipient = TUSB_REQ_RCPT_INTERFACE,
                        .type      = TUSB_REQ_TYPE_CLASS,
                        .direction = TUSB_DIR_IN
                },
                .bRequest = CDC_NOTIF_CONNECTION_SPEED_CHANGE,
                .wLength  = 8,
        },
        .downlink = 1000000,
        .uplink   = 1000000,
};



static void notification_xmt(uint8_t rhport, bool force_next)
/**
 * Transmit next notification to the host (if appropriate).
 * Notifications are transferred to the host once during connection setup.
 */
{
    printf("notification_xmt(%d, %d) - %d %d\n", force_next, rhport, ncm_interface.notification_xmt_state, ncm_interface.notification_xmt_is_running);

    if ( !force_next  &&  ncm_interface.notification_xmt_is_running) {
        return;
    }

    if (ncm_interface.notification_xmt_state == NOTIFICATION_SPEED) {
        printf("  NOTIFICATION_SPEED\n");
        ncm_notify_speed_change.header.wIndex = ncm_interface.itf_num;
        usbd_edpt_xfer(rhport, ncm_interface.ep_notif, (uint8_t*) &ncm_notify_speed_change, sizeof(ncm_notify_speed_change));
        ncm_interface.notification_xmt_state = NOTIFICATION_CONNECTED;
        ncm_interface.notification_xmt_is_running = true;
    }
    else if (ncm_interface.notification_xmt_state == NOTIFICATION_CONNECTED) {
        printf("  NOTIFICATION_CONNECTED\n");
        ncm_notify_connected.header.wIndex = ncm_interface.itf_num;
        usbd_edpt_xfer(rhport, ncm_interface.ep_notif, (uint8_t*) &ncm_notify_connected, sizeof(ncm_notify_connected));
        ncm_interface.notification_xmt_state = NOTIFICATION_DONE;
        ncm_interface.notification_xmt_is_running = true;
    }
    else {
        printf("  NOTIFICATION_FINISHED\n");
    }
}   // notification_xmt


//-----------------------------------------------------------------------------
//
// everything about packet transmission (driver -> TinyUSB)
//


static void xmt_free_current_buffer(void)
{
    printf("xmt_free_current_buffer()\n");
}   // xmt_free_current_buffer



static bool xmt_insert_required_zlp(uint8_t rhport)
{
    printf("xmt_insert_required_zlp(%d)\n", rhport);
#if 0
    if (xferred_bytes != 0  &&  xferred_bytes % CFG_TUD_NET_ENDPOINT_SIZE == 0)
    {
        // TODO check when ZLP is really needed
        ncm_interface.xmt_running = true;
        usbd_edpt_xfer(rhport, ncm_interface.ep_in, NULL, 0);
    }
#endif
    return false;
}   // xmt_insert_required_zlp



static void xmt_start_if_possible(void)
{
    printf("xmt_start_if_possible()\n");
#if 0
    // If there are datagrams queued up that we tried to send while this NTB was being emitted, send them now
    if (ncm_interface.datagram_count && ncm_interface.itf_data_alt == 1) {
        ncm_start_tx();
    }
#endif
}   // xmt_start_if_possible


//-----------------------------------------------------------------------------
//
// all the tud_network_*() stuff (glue logic -> driver)
//


bool tud_network_can_xmit(uint16_t size)
{
    printf("tud_network_can_xmit(%d)\n", size);
    return false;
}   // tud_network_can_xmit



void tud_network_xmit(void *ref, uint16_t arg)
{
    printf("tud_network_xmit(%p, %d)\n", ref, arg);
}   // tud_network_xmit



void tud_network_recv_renew(void)
{
    printf("tud_network_recv_renew()\n");

    if ( !usbd_edpt_busy(ncm_interface.rhport, ncm_interface.ep_out)) {
        printf("   may start reception\n");
        return;
    }
}   // tud_network_recv_renew



void tud_network_recv_renew_r(uint8_t rhport)
{
    printf("tud_network_recv_renew_r(%d)\n", rhport);

    ncm_interface.rhport = rhport;    // TODO better place...
    tud_network_recv_renew();
}   // tud_network_recv_renew


//-----------------------------------------------------------------------------
//
// all the netd_*() stuff (interface TinyUSB -> driver)
//
void netd_init(void)
/**
 * Initialize the driver data structures.
 * Might be called several times.
 */
{
    printf("netd_init()\n");

    memset( &ncm_interface, 0, sizeof(ncm_interface));
}   // netd_init



void netd_reset(uint8_t rhport)
/**
 * Resets the port.
 * In this driver this is the same as netd_init()
 */
{
    printf("netd_reset(%d)\n", rhport);

    netd_init();
}   // netd_reset



uint16_t netd_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len)
/**
 * Open the USB interface.
 * - parse the USB descriptor \a TUD_CDC_NCM_DESCRIPTOR for itfnum and endpoints
 * - a specific order of elements in the descriptor is tested.
 *
 * \note
 *   Actually all of the information could be read directly from \a itf_desc, because the
 *   structure and the values are well known.  But we do it this way.
 *
 * \post
 * - \a itf_num set
 * - \a ep_notif, \a ep_in and \a ep_out are set
 * - USB interface is open
 */
{
    printf("netd_open(%d,%p,%d)\n", rhport, itf_desc, max_len);

    TU_ASSERT(ncm_interface.ep_notif == 0, 0);           // assure that the interface is only opened once

    ncm_interface.itf_num = itf_desc->bInterfaceNumber;  // management interface

    //
    // skip the two first entries and the following TUSB_DESC_CS_INTERFACE entries
    //
    uint16_t drv_len = sizeof(tusb_desc_interface_t);
    uint8_t const *p_desc = tu_desc_next(itf_desc);
    while (tu_desc_type(p_desc) == TUSB_DESC_CS_INTERFACE  &&  drv_len <= max_len) {
        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    //
    // get notification endpoint
    //
    TU_ASSERT(tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT, 0);
    TU_ASSERT(usbd_edpt_open(rhport, (tusb_desc_endpoint_t const* ) p_desc), 0);
    ncm_interface.ep_notif = ((tusb_desc_endpoint_t const*) p_desc)->bEndpointAddress;
    drv_len += tu_desc_len(p_desc);
    p_desc = tu_desc_next(p_desc);

    //
    // skip the following TUSB_DESC_INTERFACE entries (which must be TUSB_CLASS_CDC_DATA)
    //
    while (tu_desc_type(p_desc) == TUSB_DESC_INTERFACE  &&  drv_len <= max_len) {
        tusb_desc_interface_t const *data_itf_desc = (tusb_desc_interface_t const*)p_desc;
        TU_ASSERT(data_itf_desc->bInterfaceClass == TUSB_CLASS_CDC_DATA, 0);

        drv_len += tu_desc_len(p_desc);
        p_desc = tu_desc_next(p_desc);
    }

    //
    // a TUSB_DESC_ENDPOINT (actually two) must follow, open these endpoints
    //
    TU_ASSERT(tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT, 0);
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, 2, TUSB_XFER_BULK, &ncm_interface.ep_out, &ncm_interface.ep_in));
    drv_len += 2 * sizeof(tusb_desc_endpoint_t);

    return drv_len;
}   // netd_open



bool netd_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes)
/**
 * Handle TinyUSB requests to process transfer events.
 */
{
    printf("netd_xfer_cb(%d,%d,%d,%u)\n", rhport, ep_addr, result, (unsigned)xferred_bytes);

    if (ep_addr == ncm_interface.ep_out) {
        //
        // new NTB received
        // - make the NTB valid
        // - if ready transfer datagrams to the glue logic for further processing
        // - if there is a free receive buffer, initiate reception
        //
        printf("  EP_OUT %d %d %d %u\n", rhport, ep_addr, result, (unsigned)xferred_bytes);
        // TODO handle_incoming_datagram(xferred_bytes);
    }
    else if (ep_addr == ncm_interface.ep_in) {
        //
        // transmission of an NTB finished
        // - free the transmitted NTB buffer
        // - insert ZLPs when necessary
        // - if there is another transmit NTB waiting, try to start transmission
        //
        printf("  EP_IN %d\n", ncm_interface.itf_data_alt);
        xmt_free_current_buffer();
        if ( !xmt_insert_required_zlp(rhport)) {
            xmt_start_if_possible();
        }
    }
    else if (ep_addr == ncm_interface.ep_notif) {
        //
        // next transfer on notification channel
        //
        printf("  EP_NOTIF\n");
        notification_xmt(rhport, true);
    }

    return true;
}   // netd_xfer_cb



bool netd_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
/**
 * Respond to TinyUSB control requests.
 * At startup transmission of notification packets are done here.
 */
{
    printf("netd_control_xfer_cb(%d, %d, %p)\n", rhport, stage, request);

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    switch (request->bmRequestType_bit.type) {
        case TUSB_REQ_TYPE_STANDARD:
            switch (request->bRequest) {
                case TUSB_REQ_GET_INTERFACE: {
                    TU_VERIFY(ncm_interface.itf_num + 1 == request->wIndex);

                    printf("  TUSB_REQ_GET_INTERFACE - %d\n", ncm_interface.itf_data_alt);
                    tud_control_xfer(rhport, request, &ncm_interface.itf_data_alt, 1);
                }
                break;

                case TUSB_REQ_SET_INTERFACE: {
                    TU_VERIFY(ncm_interface.itf_num + 1 == request->wIndex  &&  request->wValue < 2);

                    ncm_interface.itf_data_alt = request->wValue;
                    printf("  TUSB_REQ_SET_INTERFACE - %d %d %d\n", ncm_interface.itf_data_alt, request->wIndex, ncm_interface.itf_num);

                    if (ncm_interface.itf_data_alt == 1) {
                        tud_network_recv_renew_r(rhport);
                        notification_xmt(rhport, false);
                    }
                    tud_control_status(rhport, request);
                }
                break;

                // unsupported request
                default:
                    return false;
            }
            break;

        case TUSB_REQ_TYPE_CLASS:
            TU_VERIFY(ncm_interface.itf_num == request->wIndex);

            printf("  TUSB_REQ_TYPE_CLASS: %d\n", request->bRequest);

            if (request->bRequest == NCM_GET_NTB_PARAMETERS) {
                // transfer NTP parameters to host.
                // TODO can one assume, that tud_control_xfer() succeeds?
                printf("    NCM_GET_NTB_PARAMETERS\n");
                tud_control_xfer(rhport, request, (void*) (uintptr_t) &ntb_parameters, sizeof(ntb_parameters));
            }
            break;

            // unsupported request
        default:
            return false ;
    }

    return true;
}   // netd_control_xfer_cb
