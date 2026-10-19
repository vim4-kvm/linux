#!/usr/bin/env python3
"""Compile actual CRG allocation/IRQ code with deterministic failure injection."""
import pathlib
import subprocess
import tempfile

text = pathlib.Path(__file__).with_name("crg_udc.c").read_text()


def function(name):
    start = text.index("static int " + name + "(")
    return text[start:text.index("\n}\n", start) + 3]


harness = r"""
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
#define CRG_RING_NUM 1
#define CRG_U3DC_CTRL_INT_EN 1
#define CRG_U3DC_CTRL_RUN 2
#define CRG_U3DC_CFG0_MAXSPEED_FS 1
#define CRG_U3DC_CFG0_MAXSPEED_HS 2
#define CRG_U3DC_CFG0_MAXSPEED_SS 3
#define CRG_U3DC_CFG0_MAXSPEED_SSP 4
#define GFP_ATOMIC 0
#define IRQF_SHARED 1
#define CRG_DEBUG(...) do {} while (0)
#define CRG_ERROR(...) do {} while (0)
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
enum { USB_SPEED_UNKNOWN, USB_SPEED_FULL, USB_SPEED_HIGH,
       USB_SPEED_SUPER, USB_SPEED_SUPER_PLUS };
struct usb_request { int unused; };
struct usb_ep { int unused; };
struct crg_udc_request { struct usb_request usb_req; };
struct crg_uccr { u32 control, capability, config0; };
struct crg_gadget_dev {
    struct crg_uccr *uccr;
    struct { int max_speed, irq; } gadget;
    struct { struct usb_ep usb_ep; } udc_ep[1];
    struct crg_udc_request *status_req;
    int irq;
};
struct platform_device { int unused; };
static struct crg_gadget_dev crg_udc_dev;
static int fail_at, stage, irq_result, request_result, irq_requests;
static struct crg_udc_request status_request;
static u32 reg_read(u32 *p) { return *p; }
static void reg_write(u32 *p, u32 v) { *p = v; }
static int init_event_ring(struct crg_gadget_dev *d, int i)
{ (void)d; (void)i; return ++stage == fail_at ? -ENOMEM : 0; }
static int init_device_context(struct crg_gadget_dev *d)
{ (void)d; return ++stage == fail_at ? -ENOMEM : 0; }
static struct usb_request *crg_udc_alloc_request(struct usb_ep *e, int f)
{ (void)e; (void)f; return ++stage == fail_at ? NULL : &status_request.usb_req; }
static int platform_get_irq(struct platform_device *p, int i)
{ (void)p; (void)i; return irq_result; }
static int crg_udc_common_irq(int irq, void *p)
{ (void)irq; (void)p; return 0; }
static int request_irq(int irq, int (*fn)(int, void *), int flags,
                       const char *name, void *data)
{
    (void)flags; (void)name;
    assert(irq == 21 && fn == crg_udc_common_irq && data == &crg_udc_dev);
    /* UDC core must know the IRQ before a gadget can bind. */
    assert(crg_udc_dev.gadget.irq == irq);
    irq_requests++;
    return request_result;
}
"""

checks = r"""
int main(void)
{
    struct platform_device device = {0};
    struct crg_uccr regs = {0};
    for (fail_at = 1; fail_at <= 3; fail_at++) {
        struct crg_gadget_dev udc = { .uccr = &regs,
            .gadget.max_speed = USB_SPEED_HIGH };
        stage = 0;
        assert(reset_data_struct(&udc) == -ENOMEM);
        assert(stage == fail_at && udc.status_req == NULL);
    }
    fail_at = 0;
    stage = 0;
    struct crg_gadget_dev udc = { .uccr = &regs,
        .gadget.max_speed = USB_SPEED_HIGH };
    assert(reset_data_struct(&udc) == 0);
    assert(udc.status_req == &status_request && stage == 3);
    /* A reset reuses the status request instead of exhausting its pool. */
    stage = 0;
    assert(reset_data_struct(&udc) == 0 && stage == 2);
    irq_result = -517; /* EPROBE_DEFER must survive unchanged. */
    assert(crg_gadget_irq_init(&device) == -517 && irq_requests == 0);
    irq_result = 21;
    request_result = -ENOMEM;
    assert(crg_gadget_irq_init(&device) == -ENOMEM && irq_requests == 1);
    request_result = 0;
    assert(crg_gadget_irq_init(&device) == 0 && irq_requests == 2);
    puts("CRG allocation and IRQ failure regressions passed");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="crg-probe-test-") as directory:
    path = pathlib.Path(directory)
    (path / "test.c").write_text(harness + function("reset_data_struct")
                                 + function("crg_gadget_irq_init") + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
