#!/usr/bin/env python3
"""Exercise actual HID unbind across retained configfs function instances."""
import pathlib
import subprocess
import tempfile

source = pathlib.Path(__file__).with_name('f_hid.c').read_text()
start = source.index('static void hidg_unbind(')
unbind = source[start:source.index('\n}\n', start) + 3]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
struct usb_request { bool live; };
struct usb_ep { int unused; };
struct usb_gadget { struct usb_ep *ep0; };
struct usb_composite_dev { struct usb_gadget *gadget; };
struct usb_configuration { struct usb_composite_dev *cdev; };
struct usb_function { int unused; };
struct f_hidg {
    struct usb_function func;
    void *cdev, *workqueue;
    int dev;
    struct usb_request *get_req;
};
#define func_to_hidg(p) ((struct f_hidg *)((char *)(p) - offsetof(struct f_hidg, func)))
static bool worker_drained;
static unsigned int allocated;
static void cdev_device_del(void *cdev, int *dev) { (void)cdev; (void)dev; }
static void destroy_workqueue(void *work) { (void)work; worker_drained = true; }
static void usb_free_all_descriptors(struct usb_function *f) { (void)f; }
static void usb_ep_free_request(struct usb_ep *ep, struct usb_request *req)
{
    assert(ep && req && req->live && worker_drained);
    req->live = false;
    allocated--;
}
'''
checks = r'''
int main(void)
{
    struct usb_ep ep = {0};
    struct usb_gadget gadget = { .ep0 = &ep };
    struct usb_composite_dev cdev = { .gadget = &gadget };
    struct usb_configuration config = { .cdev = &cdev };
    struct f_hidg hid = {0};
    struct usb_request request = {0};
    /* CRG has a finite request pool. Retaining the function instance must
     * not accumulate one idle GET_REPORT request per binding.
     */
    for (int cycle = 0; cycle < 500; cycle++) {
        worker_drained = false;
        request.live = true;
        hid.get_req = &request;
        allocated++;
        hidg_unbind(&config, &hid.func);
        assert(!allocated && !hid.get_req && !request.live);
    }
    /* disable() may already have released a pending userspace request. */
    hidg_unbind(&config, &hid.func);
    assert(!allocated);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='hid-request-test-') as directory:
    path = pathlib.Path(directory)
    (path / 'test.c').write_text(harness + unbind + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('HID request lifecycle regressions passed')
