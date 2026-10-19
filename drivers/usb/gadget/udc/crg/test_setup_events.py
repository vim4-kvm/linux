#!/usr/bin/env python3
"""Compile the actual event dispatcher against a small EP0 lifecycle harness.

Run with an optional crg_udc.c path to check an older revision as a negative
control. No hardware is accessed; MMIO and completion handlers are stubbed.
"""
import pathlib
import subprocess
import sys
import tempfile

source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).with_name("crg_udc.c")
text = source.read_text()
start = text.index("int crg_udc_handle_event(")
end = text.index("\nstatic int process_event_ring(", start)
dispatcher = text[start:end]

harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
enum { USB_STATE_ATTACHED = 1, USB_STATE_POWERED,
       USB_STATE_RECONNECTING, USB_STATE_UNAUTHENTICATED,
       USB_STATE_DEFAULT, USB_STATE_ADDRESS, USB_STATE_CONFIGURED };
enum { WAIT_FOR_SETUP, STATUS_STAGE_XFER };
enum { TRB_TYPE_EVT_PORT_STATUS_CHANGE = 1, TRB_TYPE_EVT_TRANSFER,
       TRB_TYPE_EVT_SETUP_PKT };
#define CRG_DEBUG(...) ((void)0)
#define CRG_ERROR(...) ((void)0)
#define GETF(field, value) (value)
struct usb_ctrlrequest { unsigned int unused; };
struct event_trb_s { unsigned int dw0, dw1, dw2, dw3; };
struct crg_gadget_dev {
    int device_state, portsc_on_reconnecting, setup_status;
    bool status_pending, async_cb_flag;
    int setups, completions, deferred, ports;
};
static int crg_handle_port_status(struct crg_gadget_dev *d, unsigned long f)
{ (void)f; d->ports++; return 0; }
static void crg_handle_xfer_event(struct crg_gadget_dev *d,
                                struct event_trb_s *e, unsigned long f)
{ (void)e; (void)f; d->status_pending = false; d->completions++; }
static void queue_setup_pkt(struct crg_gadget_dev *d,
                            struct usb_ctrlrequest *s, u8 tag)
{ (void)s; (void)tag; d->deferred++; }
static void crg_handle_setup_pkt(struct crg_gadget_dev *d,
                                 struct usb_ctrlrequest *s, u8 tag,
                                 unsigned long f)
{
    (void)s; (void)tag; (void)f;
    /* A second use of the still-owned status request is the real panic. */
    assert(!d->status_pending);
    d->status_pending = true;
    d->setup_status = STATUS_STAGE_XFER;
    d->setups++;
}
"""
checks = r"""
int main(void)
{
    struct event_trb_s setup = { .dw3 = TRB_TYPE_EVT_SETUP_PKT };
    struct event_trb_s complete = { .dw3 = TRB_TYPE_EVT_TRANSFER };
    struct event_trb_s port = { .dw3 = TRB_TYPE_EVT_PORT_STATUS_CHANGE };
    struct crg_gadget_dev d = {
        .device_state = USB_STATE_RECONNECTING,
        .setup_status = WAIT_FOR_SETUP,
        .status_pending = true,
        .async_cb_flag = true,
    };
    /* A reset resets setup_status before the old EP0 request is retired. */
    for (int i = 0; i < 20; i++)
        assert(crg_udc_handle_event(&d, &setup, 0) == 0);
    assert(d.status_pending && d.setups == 0 && d.deferred == 0);
    crg_udc_handle_event(&d, &port, 0);
    assert(d.portsc_on_reconnecting == 1 && d.ports == 0);
    crg_udc_handle_event(&d, &complete, 0);
    assert(!d.status_pending && d.completions == 1);

    /* After prepare_for_setup publishes DEFAULT, enumeration resumes. */
    d.device_state = USB_STATE_DEFAULT;
    crg_udc_handle_event(&d, &setup, 0);
    assert(d.setups == 1 && d.status_pending);
    crg_udc_handle_event(&d, &setup, 0);
    assert(d.setups == 1 && d.deferred == 1);
    crg_udc_handle_event(&d, &complete, 0);
    assert(!d.status_pending && d.completions == 2);

    /* Detached/powered controllers must not start control transfers. */
    for (int state = 0; state < USB_STATE_DEFAULT; state++) {
        struct crg_gadget_dev idle = { .device_state = state, .async_cb_flag = true };
        crg_udc_handle_event(&idle, &setup, 0);
        assert(idle.setups == 0 && idle.deferred == 0);
    }
    /* Unbind may disable callbacks before the device state changes. No
     * status/stall request may be submitted across controller teardown. */
    for (int state = USB_STATE_DEFAULT; state <= USB_STATE_CONFIGURED; state++) {
        struct crg_gadget_dev unbinding = { .device_state = state,
                                           .status_pending = true };
        crg_udc_handle_event(&unbinding, &setup, 0);
        assert(unbinding.setups == 0 && unbinding.deferred == 0);
        crg_udc_handle_event(&unbinding, &complete, 0);
        assert(!unbinding.status_pending && unbinding.completions == 1);
    }
    puts("EP0 reset/setup event regression passed");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="crg-setup-test-") as directory:
    path = pathlib.Path(directory)
    (path / "test.c").write_text(harness + dispatcher + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
