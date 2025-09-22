# tidbit gateron firmware

Firmware for tidbit-like numpad with Gateron switches and RP2040-Zero.

Hardware: https://github.com/dseight/tidbit-gateron

`usbd_init.c` is copied from Zephyr's `samples/subsys/usb/common/sample_usbd_init.c`.

## Building

    west build -b rp2040_zero

## License

Licensed under [Apache-2.0](LICENSE).
