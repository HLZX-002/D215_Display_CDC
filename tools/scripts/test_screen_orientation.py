"""Configuration, SN and actual C coordinate regression tests (Python 3).

Uses the SDK's kconfiglib and RISC-V compiler. With --native, also executes
the C transforms via a host C compiler (CC, or `pip install ziglang`).
No project configuration is overwritten.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'kernel/rt-thread/tools'))
import kconfiglib
from screen_profiles import TABLE, OUTPUT, render

BOARDS = ['SY_L06'] * 6 + ['SY701_D2'] * 3 + ['HL1409'] * 2 + ['HL1102',
          'HL6067'] + ['HL6011'] * 3 + ['HL6126'] * 3


def config_header(k):
    result = []
    names = []
    for sym in k.unique_defined_syms:
        line = sym.config_string
        if line.startswith('CONFIG_'):
            name, value = line[7:].strip().split('=', 1)
            names.append(name)
            result.append('#define ' + name + (' 1' if value == 'y' else ' ' + value))
    return '\n'.join(result), names


def configure(k, p, board):
    # Clear the dependent screen choice before switching its parent board.
    for profile in json.loads(TABLE.read_text(encoding='utf-8')):
        k.syms[profile['symbol']].set_value(0)
    k.syms['AIC_BOARD_' + board].set_value(2)
    k.syms[p['symbol']].set_value(2)
    assert k.syms[p['symbol']].str_value == 'y'


def checked(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError('Compiler failed: ' + str(command[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    os.chdir(ROOT)
    os.environ.update(RTT_DIR='kernel/rt-thread', PKGS_DIR='tools/env/packages')
    profiles = json.loads(TABLE.read_text(encoding='utf-8'))
    assert OUTPUT.read_text(encoding='utf-8') == render(profiles)
    k = kconfiglib.Kconfig(warn_to_stderr=False)
    k.load_config('.config')
    k.syms['AIC_SCREEN_UNIFIED_ORIENTATION'].set_value(0)
    legacy_fb = k.syms['AIC_FB_ROTATE_DEGREE'].str_value
    legacy_lv = k.syms['LV_DISPLAY_ROTATE_EN'].str_value
    source = ['#include <stdint.h>', '#define EXPORT __attribute__((dllexport))',
              '#define RT_TOUCH_EVENT_DOWN 1', '#define RT_TOUCH_EVENT_MOVE 2',
              '#define RT_TOUCH_EVENT_UP 3', '#define RT_TOUCH_CTRL_GET_DYNAMIC_CROP 1',
              'struct rt_touch_data { int event; uint16_t x_coordinate, y_coordinate; };',
              'struct rt_touch_crop_info { int enable, width, height; };',
              'struct hid_touch_t { void *dev; struct { unsigned point_num; } info; struct rt_touch_data *data; int x_physical_maxnum, y_physical_maxnum; };',
              'static struct rt_touch_crop_info requested_crop;',
              'static int rt_device_control(void *dev, int cmd, struct rt_touch_crop_info *crop) { *crop=requested_crop; return 0; }']
    hid_source = (ROOT / 'packages/third-party/cherryusb/demo/usbd_hid_touch_template.c').read_text(encoding='utf-8')
    # Compile the actual frame transform with minimal RT-Thread I/O stubs.
    start = hid_source.index('static void product_hid_transform_frame(')
    hid_transform = hid_source[start:hid_source.index('\n#endif', start)]
    cases = []
    old_names = []
    for p, board in zip(profiles, BOARDS):
        configure(k, p, board)
        k.syms['AIC_SCREEN_UNIFIED_ORIENTATION'].set_value(2)
        for letter in 'A':
            for angle in (0, 90, 180, 270):
                k.syms['AICFB_ROTATE_' + str(angle)].set_value(2)
                width, height = (p['height'], p['width']) if angle % 180 else (p['width'], p['height'])
                expected = {
                    'AIC_SCREEN_LOGICAL_WIDTH': str(width),
                    'AIC_SCREEN_LOGICAL_HEIGHT': str(height),
                    'AIC_FB_ROTATE_DEGREE': str(angle),
                    'AIC_FB_ROTATE_EN': 'y' if angle else 'n',
                    'LV_DISPLAY_ROTATE_EN': 'n',
                    'AIC_SCREEN_ORIENTATION_LETTER': letter,
                    'AIC_USB_DISP_CALIBRATION_DEGREE': '0',
                }
                for name, value in expected.items():
                    assert k.syms[name].str_value == value, (p['symbol'], letter, angle, name, k.syms[name].str_value)
                native = ('PANEL_HACTIVE', 'PANEL_VACTIVE') if board == 'HL6126' else ('AIC_JD9365_HACTIVE', 'AIC_JD9365_VACTIVE')
                assert [int(k.syms[name].str_value) for name in native] == [p['width'], p['height']]
                k.syms['LV_DISPLAY_ROTATE_EN'].set_value(2)
                assert k.syms['LV_DISPLAY_ROTATE_EN'].str_value == 'n'
                header, names = config_header(k)
                n = len(cases)
                source += ['#undef ' + name for name in old_names]
                source += ['#undef AIC_SCREEN_ORIENTATION_H', '#undef USB_CDC_SCREEN_SN_CONFIG_H',
                           '#undef aic_screen_touch_to_logical',
                           '#define aic_screen_touch_to_logical touch_%d' % n,
                           '#undef product_hid_transform_frame',
                           '#define product_hid_transform_frame frame_%d' % n,
                           '#undef product_hid_angle',
                           '#define product_hid_angle angle_%d' % n,
                           header, '#include "screen_sn_config.h"',
                           'static unsigned int product_hid_angle;', hid_transform,
                           'typedef char sn_size_%d[(sizeof(USB_CDC_DEFAULT_SN) == 27) ? 1 : -1];' % n,
                           'EXPORT const char *sn_%d(void) { return USB_CDC_DEFAULT_SN; }' % n,
                           'EXPORT void point_%d(int16_t *x, int16_t *y) { touch_%d(x, y); }' % (n, n),
                           ('EXPORT void hid_%d(int *v) { struct rt_touch_data data={RT_TOUCH_EVENT_DOWN, v[0], v[1]}; '
                            'struct hid_touch_t hid={0}; hid.info.point_num=1; hid.data=&data; '
                            'product_hid_angle=v[2]; requested_crop.enable=v[3]!=0; requested_crop.width=v[3]; requested_crop.height=v[4]; '
                            'product_hid_transform_frame(&hid); v[0]=data.x_coordinate; v[1]=data.y_coordinate; '
                            'v[2]=hid.x_physical_maxnum; v[3]=hid.y_physical_maxnum; v[4]=data.event; }') % n]
                old_names = names
                cases.append((p, letter, angle, width, height))
    # Toggle back to legacy without losing its stored angle choices.
    k.load_config('.config')
    k.syms['AIC_SCREEN_UNIFIED_ORIENTATION'].set_value(0)
    assert k.syms['AIC_FB_ROTATE_DEGREE'].str_value == legacy_fb
    assert k.syms['LV_DISPLAY_ROTATE_EN'].str_value == legacy_lv
    with tempfile.TemporaryDirectory(prefix='screen-orientation-') as work:
        work = Path(work)
        (work / 'rtconfig.h').write_text('/* configuration supplied by probe */\n')
        probe = work / 'probe.c'
        probe.write_text('\n'.join(source), encoding='utf-8')
        includes = ['-I' + str(work), '-I' + str(ROOT / 'bsp/artinchip/include/uapi'),
                    '-I' + str(ROOT / 'application/rt-thread/helloworld')]
        gcc = ROOT / 'toolchain/bin/riscv64-unknown-elf-gcc.exe'
        checked([str(gcc), '-fsyntax-only', '-Wno-attributes', *includes, str(probe)])
        print('PASS: %d screens x 4 display rotations; C contracts and SN lengths' % len(profiles))
        if args.native:
            if os.environ.get('CC'):
                cc = shlex.split(os.environ['CC'])
            else:
                import ziglang
                cc = [str(Path(ziglang.__path__[0]) / 'zig.exe'), 'cc']
            dll_path = work / 'probe.dll'
            checked([*cc, '-shared', '-O2', *includes, str(probe), '-o', str(dll_path)])
            dll = ctypes.CDLL(str(dll_path))
            for n, (p, letter, angle, width, height) in enumerate(cases):
                sn = getattr(dll, 'sn_%d' % n)
                sn.restype = ctypes.c_char_p
                assert sn().decode() == 'VMAX%s%s%04d*%04dS260100001' % (letter, p['model'], width, height)
                point = getattr(dll, 'point_%d' % n)
                point.argtypes = [ctypes.POINTER(ctypes.c_int16)] * 2
                corners = set()
                for x in (0, p['touch_width'] - 1):
                    for y in (0, p['touch_height'] - 1):
                        px, py = ctypes.c_int16(x), ctypes.c_int16(y)
                        point(ctypes.byref(px), ctypes.byref(py))
                        corners.add((px.value, py.value))
                assert corners == {(0, 0), (width-1, 0), (0, height-1), (width-1, height-1)}, (p, letter, corners)
                # The physical marker for the decoded top-left corner is
                # calculated separately with normalized coordinates.
                x, y = (1 if p.get('touch_flip_x') else 0), (1 if p.get('touch_flip_y') else 0)
                turns = ((p['touch_to_native_cw'] - angle) % 360) // 90
                for _ in range(turns):
                    x, y = 1-y, x
                px, py = ctypes.c_int16(0), ctypes.c_int16(0)
                point(ctypes.byref(px), ctypes.byref(py))
                assert (px.value, py.value) == (x*(width-1), y*(height-1))
                for raw in [(-100, -100), (32767, 32767), (p['touch_width']//2, p['touch_height']//2)]:
                    px, py = ctypes.c_int16(raw[0]), ctypes.c_int16(raw[1])
                    point(ctypes.byref(px), ctypes.byref(py))
                    assert 0 <= px.value < width and 0 <= py.value < height
                hid = getattr(dll, 'hid_%d' % n)
                hid.argtypes = [ctypes.POINTER(ctypes.c_int)]
                for requested_angle in (0, 90, 180, 270):
                    hw, hh = (p['height'], p['width']) if requested_angle % 180 else (p['width'], p['height'])
                    x, y = 0, 0
                    for _ in range(((angle-requested_angle) % 360)//90):
                        x, y = 1-y, x
                    v = (ctypes.c_int * 5)(0, 0, requested_angle, 0, 0)
                    hid(v)
                    assert list(v) == [x*(hw-1), y*(hh-1), hw-1, hh-1, 1]
                    # Outside a centered crop releases the contact and clamps;
                    # a center touch stays down. Invalid crops are ignored.
                    for cw, ch, outside in [(hw//2, hh//2, True), (1, 1, False), (hw+1, hh+1, False)]:
                        v = (ctypes.c_int * 5)(0, 0, requested_angle, cw, ch)
                        hid(v)
                        assert v[4] == (3 if outside else 1)
                        assert 0 <= v[0] < hw and 0 <= v[1] < hh
                    v = (ctypes.c_int * 5)(width//2, height//2, requested_angle, hw//2, hh//2)
                    hid(v)
                    assert v[4] == 1
            # Release the DLL before TemporaryDirectory removes it on Windows.
            import _ctypes
            _ctypes.FreeLibrary(dll._handle)
            print('PASS: native C execution, 64 SN strings, four corners, rotation sense and bounds')
            print('PASS: actual HID frame transform, 256 product/host angle pairs and crop bounds')


if __name__ == '__main__':
    main()
