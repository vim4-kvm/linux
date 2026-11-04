#!/usr/bin/env python3
"""Run the production regulator paths with fake supplies, without PD I/O."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('fusb302.c').read_text()


def function(name):
    start = source.index('static int ' + name + '(')
    return source[start:source.index('\n}\n', start) + 3]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define EPROBE_DEFER 517
struct device { int unused; };
struct fwnode_handle { const char *role; };
struct regulator { int unused; };
struct tcpc_dev { int unused; };
struct fusb302_chip {
    struct tcpc_dev tcpc_dev;
    struct device *dev;
    struct regulator *vbus;
    bool vbus_on, charge_on;
    int lock;
};
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define ERR_PTR(e) ((struct regulator *)(intptr_t)(e))
#define PTR_ERR_OR_ZERO(p) ((intptr_t)(p) < 0 ? (int)(intptr_t)(p) : 0)
#define fusb302_log(...) ((void)0)
static struct fwnode_handle node;
static struct regulator supply;
static int supply_error, optional_gets, required_gets, enabled, disabled, puts;
static bool has_connector = true;
static struct fwnode_handle *device_get_named_child_node(struct device *d, const char *n)
{ (void)d; assert(!strcmp(n, "connector")); return has_connector ? &node : NULL; }
static int fwnode_property_read_string(struct fwnode_handle *f, const char *key, const char **v)
{ assert(!strcmp(key, "power-role")); if (!f || !f->role) return -EINVAL; *v=f->role; return 0; }
static void fwnode_handle_put(struct fwnode_handle *f) { if (f) puts++; }
static struct regulator *get_supply(const char *n)
{ assert(!strcmp(n, "vbus")); return supply_error ? ERR_PTR(supply_error) : &supply; }
static struct regulator *devm_regulator_get_optional(struct device *d, const char *n)
{ (void)d; optional_gets++; return get_supply(n); }
static struct regulator *devm_regulator_get(struct device *d, const char *n)
{ (void)d; required_gets++; return get_supply(n); }
static int regulator_enable(struct regulator *r) { assert(r == &supply); enabled++; return 0; }
static int regulator_disable(struct regulator *r) { assert(r == &supply); disabled++; return 0; }
static void mutex_lock(int *l) { assert(!*l); *l=1; }
static void mutex_unlock(int *l) { assert(*l); *l=0; }
'''
checks = r'''
int main(void)
{
    struct device dev;
    struct fusb302_chip chip = { .dev = &dev };
    node.role = "sink";
    supply_error = -ENODEV;
    assert(fusb302_get_vbus(&chip) == 0 && chip.vbus == NULL);
    assert(optional_gets == 1 && required_gets == 0 && puts == 1);
    assert(tcpm_set_vbus(&chip.tcpc_dev, false, true) == 0);
    assert(chip.charge_on && !chip.vbus_on && enabled == 0 && disabled == 0);
    assert(tcpm_set_vbus(&chip.tcpc_dev, true, false) == -EOPNOTSUPP);
    assert(chip.charge_on && !chip.vbus_on && !chip.lock);
    assert(tcpm_set_vbus(&chip.tcpc_dev, false, false) == 0 && !chip.charge_on);
    supply_error = -EPROBE_DEFER;
    assert(fusb302_get_vbus(&chip) == -EPROBE_DEFER);
    supply_error = -EIO;
    assert(fusb302_get_vbus(&chip) == -EIO);
    supply_error = 0;
    assert(fusb302_get_vbus(&chip) == 0 && chip.vbus == &supply);
    node.role = "dual";
    assert(fusb302_get_vbus(&chip) == 0 && required_gets == 1);
    assert(tcpm_set_vbus(&chip.tcpc_dev, true, false) == 0 && enabled == 1);
    assert(tcpm_set_vbus(&chip.tcpc_dev, true, false) == 0 && enabled == 1);
    assert(tcpm_set_vbus(&chip.tcpc_dev, false, false) == 0 && disabled == 1);
    node.role = "source";
    supply_error = -ENODEV;
    assert(fusb302_get_vbus(&chip) == -ENODEV);
    node.role = NULL;
    assert(fusb302_get_vbus(&chip) == -ENODEV);
    has_connector = false;
    assert(fusb302_get_vbus(&chip) == -ENODEV);
    assert(required_gets == 4 && optional_gets == 4);
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    test.write_text(harness + function('fusb302_get_vbus') +
                    function('tcpm_set_vbus') + checks)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: sink without VBUS source, deferred/error supplies, source/dual-role lifecycle')
