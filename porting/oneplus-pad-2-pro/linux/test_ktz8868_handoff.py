#!/usr/bin/env python3
"""Exercise the actual patched KTZ8868 functions with register/GPIO fault injection."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    start = source.index('static int ' + name + '(')
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
#define dev_info(...) ((void)0)
struct device { int unused; };
struct i2c_client { struct device dev; };
struct ktz8866 {
    struct i2c_client *client;
    void *regmap, *enable_gpio;
    bool led_on;
    unsigned bl_en;
};
static unsigned enabled, writes, fail_write, delay9, directions, bias_writes;
static int hw_level, read_error;
static bool configure_bias, global_off, sinks_off;
static int ktz8866_write(struct ktz8866 *k, unsigned reg, unsigned value) {
    (void)k;
    if (++writes == fail_write) return -5;
    if (reg == BL_EN && !value) {
        assert(global_off && delay9);
        sinks_off = true;
    }
    if (reg == KTZ8868_BL_CTRL && !value) global_off = true;
    if (reg == LCD_BOOST_CFG || reg == OUTP_CFG || reg == OUTN_CFG || reg == LCD_BIAS_CFG1) {
        assert(!(enabled & BL_EN_BIT) || sinks_off);
        ++bias_writes;
    }
    return 0;
}
static void ktz8866_set_brightness(struct ktz8866 *k, unsigned value) {
    (void)ktz8866_write(k, BL_BRT_LSB, value & 7);
    (void)ktz8866_write(k, BL_BRT_MSB, value >> 3);
}
static void msleep(unsigned ms) {
    if (ms == 9) { assert(global_off); ++delay9; }
}
static void usleep_range(unsigned low, unsigned high) {
    assert(low == 1000 && high == 1500 && directions);
}
static int gpiod_get_value_cansleep(void *gpio) { assert(gpio); return hw_level; }
static int gpiod_direction_output(void *gpio, int value) {
    assert(gpio && value == 1); ++directions; return 0;
}
static int regmap_read(void *map, unsigned reg, unsigned *value) {
    (void)map; assert(reg == BL_EN); *value = enabled; return read_error;
}
static bool device_property_read_bool(struct device *dev, const char *name) {
    (void)dev; (void)name; return configure_bias;
}
/* DRIVER_FUNCTIONS */
static struct i2c_client client;
static struct ktz8866 reset(bool gpio, int level, bool bias) {
    writes = fail_write = delay9 = directions = bias_writes = 0;
    read_error = 0; enabled = 0x77; hw_level = level;
    configure_bias = bias; global_off = sinks_off = false;
    return (struct ktz8866){.client=&client, .enable_gpio=gpio ? &client : NULL};
}
int main(void) {
    struct ktz8866 k = reset(true, 1, true);
    assert(!ktz8868_power_up(&k));
    assert(global_off && sinks_off && !k.led_on && delay9 == 1);
    assert(!directions && !bias_writes); /* retain live HWEN and LCD bias */

    k = reset(false, 0, false); /* secondary string without HWEN GPIO */
    assert(!ktz8868_power_up(&k));
    assert(global_off && sinks_off && delay9 == 1 && !bias_writes);

    k = reset(true, 0, true); /* cold enable still configures bias */
    assert(!ktz8868_power_up(&k));
    assert(directions == 1 && bias_writes == 4 && sinks_off);

    k = reset(true, 1, true); enabled = 0;
    assert(!ktz8868_power_up(&k));
    assert(!global_off && !delay9 && !bias_writes);

    /* Any shutdown failure must prevent changing LCD bias. */
    for (unsigned i = 1; i <= 4; ++i) {
        k = reset(true, 0, true); fail_write = i;
        assert(ktz8868_power_up(&k) == -5);
        assert(!bias_writes);
    }
    k = reset(true, 0, true); read_error = -5;
    assert(ktz8868_power_up(&k) == -5 && !writes);
    k = reset(true, -5, true);
    assert(ktz8868_power_up(&k) == -5 && !writes && !directions);
    puts("KTZ8868 handoff: live master, secondary, cold, off and fault cases passed");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel-source', type=Path, required=True)
    args = parser.parse_args()
    source = (args.kernel_source / 'drivers/video/backlight/ktz8866.c').read_text()
    constants = '\n'.join(re.findall(r'^#define .+$', source, re.MULTILINE))
    functions = '\n'.join(function(source, name) for name in
                          ('ktz8868_update_brightness', 'ktz8868_power_up'))
    test = HARNESS.replace('struct device {', constants + '\nstruct device {')
    test = test.replace('/* DRIVER_FUNCTIONS */', functions)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'test.c'
        binary = Path(tmp) / 'test'
        path.write_text(test)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
