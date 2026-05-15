#!/usr/bin/env python3
import errno
import os
import select
import sys
import termios


BAUD_RATES = {
    9600: termios.B9600,
    115200: termios.B115200,
}


def configure_serial(fd: int, baud: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = BAUD_RATES[baud]
    attrs[5] = BAUD_RATES[baud]
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: serial-monitor.py /dev/cu.usbmodemXXXX 115200", file=sys.stderr)
        return 2

    port = sys.argv[1]
    baud = int(sys.argv[2])
    if baud not in BAUD_RATES:
        print(f"unsupported baud rate: {baud}", file=sys.stderr)
        return 2

    try:
        serial_fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as error:
        if error.errno == errno.EBUSY:
            print(f"{port} is already in use. Close any old screen/serial monitor window and run again.",
                  file=sys.stderr)
            return 1
        raise
    configure_serial(serial_fd, baud)

    stdin_fd = sys.stdin.fileno()
    stdout_fd = sys.stdout.fileno()

    old_stdin = None
    if not sys.stdin.isatty():
        print("This serial monitor needs an interactive terminal for typing commands.",
              file=sys.stderr)
        print("In CLion, enable 'Execute in terminal' for this run configuration,",
              file=sys.stderr)
        print("or run the same command from the Terminal tab.", file=sys.stderr)
        os.close(serial_fd)
        return 1

    old_stdin = termios.tcgetattr(stdin_fd)
    raw = termios.tcgetattr(stdin_fd)
    raw[3] &= ~(termios.ECHO | termios.ICANON)
    termios.tcsetattr(stdin_fd, termios.TCSANOW, raw)

    try:
        while True:
            readable, _, _ = select.select([serial_fd, stdin_fd], [], [])

            if serial_fd in readable:
                try:
                    data = os.read(serial_fd, 4096)
                except OSError as error:
                    if error.errno in (errno.ENXIO, errno.EIO):
                        print("\nSerial port disappeared. If this happened after pressing the Pico reset button,",
                              file=sys.stderr)
                        print("the monitor was connected to the Pico USB port instead of the Debug Probe UART.",
                              file=sys.stderr)
                        return 1
                    raise
                if data:
                    os.write(stdout_fd, data)

            if stdin_fd in readable:
                data = os.read(stdin_fd, 1024)
                if not data:
                    break
                os.write(serial_fd, data)
    finally:
        if old_stdin is not None:
            termios.tcsetattr(stdin_fd, termios.TCSANOW, old_stdin)
        os.close(serial_fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
