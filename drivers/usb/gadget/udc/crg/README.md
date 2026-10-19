# T7 CRG UDC experiment

This driver is under development. Compilation and host-side regression tests
do not establish that enumeration, DMA or hotplug works on hardware.

Enable CONFIG_USB_CRG_UDC=m and the required configfs functions. The dedicated
amlogic-t7-a311d2-khadas-vim4-udc-test.dtb replaces only the fdd00000 xHCI child
with amlogic,t7-crg-udc and puts its parent in fixed peripheral mode. The normal
VIM4 DTB is unchanged. Do not bind this driver and xHCI to the same register range.

The Meson parent owns PHY initialization, reset and the USB power domain. The
child uses its own core clock reference and a 32-bit coherent DMA mask. Probe
requires that parent and rejects OTG/host mode. No private configfs structures
or automatic gadget rebinding are used; userspace owns the UDC attribute.

Only one controller instance and USB high speed are supported. System sleep is
rejected until context restoration is implemented and tested. If neither halt
nor reset acknowledges DMA shutdown, ring memory is retained rather than freed
under the controller, and a new instance is blocked for this module lifetime.

Before deploying, review the transfer engine's inherited assumptions, especially
request completion, disconnect/rebind and reset under load. Use a distinct
kernel release and matching modules for the trial, keep the default boot entry,
and capture UART. Do not install this over a working kernel's modules.

Offline checks (C compiler and Python 3, no hardware):

```
python3 drivers/usb/gadget/udc/crg/test_setup_events.py
python3 drivers/usb/gadget/udc/crg/test_probe_failures.py
```

The tests compile actual driver functions with controlled completions/allocation
failures. They cover EP0 ownership across SETUP/reset/unbind, allocation failures
and IRQ error propagation; they do not emulate the controller or validate DMA.
