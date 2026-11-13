# Plugs in 8 virtual gamepads (ViGEmBus, pip package vgamepad) and holds pad n's left stick right at n/8 of full
# deflection and A on even pads, for a number of seconds. The test mod's "TEST pads" lines show what each port read.
# Usage: python gamepad_test.py [seconds] [x360|ds4]
import sys, time
import vgamepad as vg

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 60
kind = sys.argv[2] if len(sys.argv) > 2 else "x360"
pads = []
for n in range(1, 9):
    if kind == "ds4":
        pad = vg.VDS4Gamepad()
        pad.left_joystick_float(x_value_float=n / 8, y_value_float=0)
        if n % 2 == 0:
            pad.press_button(button=vg.DS4_BUTTONS.DS4_BUTTON_CROSS)
    else:
        pad = vg.VX360Gamepad()
        pad.left_joystick_float(x_value_float=n / 8, y_value_float=0)
        if n % 2 == 0:
            pad.press_button(button=vg.XUSB_BUTTON.XUSB_GAMEPAD_A)
    pad.update()
    pads.append(pad)
    time.sleep(0.3)  # plug in one after another so SDL numbers them in order
print(f"{len(pads)} {kind} pads plugged in", flush=True)
# SDL (RawInput) only sees changes, and a game started later never sees a state set before it opened the pad:
# wiggle each stick by a hair so a fresh report keeps coming.
end = time.time() + seconds
wiggle = 0
while time.time() < end:
    wiggle ^= 1
    for n, pad in enumerate(pads, 1):
        pad.left_joystick_float(x_value_float=n / 8 - wiggle * 0.001, y_value_float=0)
        pad.update()
    time.sleep(0.05)
