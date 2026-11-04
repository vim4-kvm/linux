#!/usr/bin/env python3
"""Exercise actual CRG bus notifications without hardware or DMA emulation."""
import pathlib
import subprocess
import tempfile

text = pathlib.Path(__file__).with_name('crg_udc.c').read_text()
start = text.index('static void crg_handle_bus_suspend(')
end = text.index('\nstatic int crg_handle_port_status(', start)
harness = r'''
#include <assert.h>
#include <stdbool.h>
enum { USB_STATE_DEFAULT = 4, USB_STATE_CONFIGURED = 6 };
static int eltest_flag, suspends, resumes, holds, drops;
struct usb_gadget { int configuration, pending_requests; };
struct driver {
    void (*suspend)(struct usb_gadget *);
    void (*resume)(struct usb_gadget *);
};
struct crg_udc_lock { bool wakesrc; };
struct crg_gadget_dev {
    bool async_cb_flag, bus_suspended;
    int device_state, udc_lock;
    struct driver *gadget_driver;
    struct usb_gadget gadget;
    struct crg_udc_lock crg_gadget_lock;
};
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (void)(flags); } while (0)
static void crg_gadget_drop(struct crg_udc_lock *l) { (void)l; drops++; }
static void crg_gadget_hold(struct crg_udc_lock *l) { (void)l; holds++; }
static void suspend_cb(struct usb_gadget *g) { assert(g->configuration == 1); suspends++; }
static void resume_cb(struct usb_gadget *g) { assert(g->pending_requests == 3); resumes++; }
'''
checks = r'''
int main(void)
{
    struct driver driver = { suspend_cb, resume_cb };
    struct crg_gadget_dev d = {
        .async_cb_flag = true, .device_state = USB_STATE_CONFIGURED,
        .gadget_driver = &driver, .gadget = {1, 3},
        .crg_gadget_lock = {true},
    };
    crg_handle_bus_suspend(&d, false, 0);
    assert(resumes == 0);
    crg_handle_bus_suspend(&d, true, 0);
    crg_handle_bus_suspend(&d, true, 0);
    assert(suspends == 1 && drops == 1 && d.bus_suspended);
    crg_handle_bus_suspend(&d, false, 0);
    crg_handle_bus_suspend(&d, false, 0);
    assert(resumes == 1 && holds == 1 && !d.bus_suspended);
    assert(d.gadget.configuration == 1 && d.gadget.pending_requests == 3);
    d.async_cb_flag = false;
    crg_handle_bus_suspend(&d, true, 0);
    assert(suspends == 1 && !d.bus_suspended);
    d.async_cb_flag = true;
    d.device_state = 1;
    crg_handle_bus_suspend(&d, true, 0);
    assert(suspends == 1);
    d.device_state = USB_STATE_CONFIGURED;
    driver.suspend = 0;
    driver.resume = 0;
    crg_handle_bus_suspend(&d, true, 0);
    crg_handle_bus_suspend(&d, false, 0);
    assert(!d.bus_suspended && suspends == 1 && resumes == 1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    source = pathlib.Path(directory) / 'test.c'
    binary = pathlib.Path(directory) / 'test'
    source.write_text(harness + text[start:end] + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('CRG bus suspend/resume notification regression passed')
