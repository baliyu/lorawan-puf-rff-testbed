#!/usr/bin/env python3
"""
Dual-SRAM PUF logger for LOW-SIDE (2N7000) configuration.

- Talks to the Arduino sketch that supports commands: C1/C2/S/N/F/T
- Captures 8,192 bytes (65,536 bits), saves:
    • Text grid of 256 rows × 32 bytes as 8-bit binary tokens (.txt)
    • Hex CSV with address + 32 bytes per row (.csv)
- Extracts 'Bias: xx.x%' from the Arduino print and stores it in filenames and stats JSON.

Requires: pip install pyserial
"""

import os
import re
import csv
import json
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("pyserial not found.\nInstall with:  pip install pyserial")

# =========================
# USER CONFIG
# =========================
PORT = "COM3"         # <-- SET THIS (e.g., COM5 on Windows, /dev/ttyACM0 on Linux, /dev/tty.usbmodem* on macOS)
BAUD = 115200

MODE = "SINGLE"   # 'SINGLE', 'DUAL_ALTERNATE', or 'SEQUENTIAL'
SINGLE_CHIP = 1          # used only if MODE == 'SINGLE' (1 or 2)

CAPTURES_PER_CHIP = 3    # how many per chip
LOOP_DELAY = 1.0          # sec between captures
OUT_BASE = "puf_logs"     # output directory root

# Serial read behavior
MAX_SECONDS = 120         # safety timeout for a capture
MAX_SILENCE = 4.0         # timeout after START if no new data appears
CAPTURE_DELAY = 0.5       # small pause after sending C1/C2

# Capture format constants
TOTAL_BYTES = 8192
ROWS, COLS = 256, 32

# Parsing helpers
BIN8 = re.compile(r"\b[01]{8}\b")
START_MARK = "=== PUF DATA START ==="
END_MARK   = "=== PUF DATA END ==="

class PUFLogger:
    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.stats = {"chip1": [], "chip2": []}

    # ----- filesystem -----
    def ensure_dirs(self):
        for chip in (1, 2):
            os.makedirs(os.path.join(OUT_BASE, f"SRAM{chip}"), exist_ok=True)
        os.makedirs(os.path.join(OUT_BASE, "analysis"), exist_ok=True)

    # ----- serial -----
    def open_serial(self):
        print(f"Opening {self.port} @ {self.baud} …")
        try:
            ser = serial.Serial(self.port, baudrate=self.baud, timeout=0.6)
        except Exception as e:
            print(f"Failed to open {self.port}: {e}")
            print("Available ports:")
            for p in list_ports.comports():
                print(f"  - {p.device}: {p.description}")
            raise
        time.sleep(1.0)
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        time.sleep(0.5)

        # Drain any banner lines
        while ser.in_waiting:
            line = ser.readline().decode("ascii", errors="ignore").strip()
            if line:
                print(f"Arduino: {line}")
        return ser

    def send_cmd(self, ser, chip):
        cmd = f"C{chip}\n".encode("ascii")
        print(f"Sending: C{chip}")
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        ser.write(cmd)
        ser.flush()
        time.sleep(CAPTURE_DELAY)

    # ----- parsing one capture from serial -----
    def read_tokens(self, ser):
        """
        Returns (tokens, bias, temp):
          tokens: list of 8192 strings, each 'xxxxxxxx' (8-bit binary)
          bias: float or None
          temp: float (deg C from DS18B20) or None
        """
        tokens = []
        started = False
        bias = None
        temp = None
        t0 = time.time()
        last_new = t0

        print("Waiting for PUF data …")
        while True:
            if time.time() - t0 > MAX_SECONDS:
                print("Capture timeout.")
                break

            try:
                line = ser.readline().decode("ascii", errors="ignore").strip()
            except Exception:
                continue

            if not line:
                if started and (time.time() - last_new > MAX_SILENCE):
                    print("Silence timeout after START.")
                    break
                continue

            # Optional: pick up Bias: 32.123% ones
            if "Bias:" in line:
                m = re.search(r"Bias:\s*([\d.]+)%", line)
                if m:
                    bias = float(m.group(1))
                    print(f"Bias detected: {bias:.3f}%")

            # Pick up Temp: 41.25C from the DS18B20
            if "Temp:" in line:
                m = re.search(r"Temp:\s*([\d.]+)", line)
                if m:
                    temp = float(m.group(1))
                    print(f"Temperature detected: {temp:.2f} C")

            # START marker or first found tokens start the capture
            if not started:
                if START_MARK in line:
                    print("Found START marker.")
                    started = True
                    last_new = time.time()
                    continue
                found = BIN8.findall(line)
                if found:
                    print("No explicit START; tokens detected, starting capture.")
                    started = True
                    tokens.extend(found)
                    last_new = time.time()
                    continue

            # If we got here, we should be in capture.
            # NOTE: the Arduino prints "Bias:" and "Temp:" AFTER this END
            # marker, so don't break here until we've actually captured the
            # temperature (or hit the final COMPLETE marker / grace timeout).
            if END_MARK in line:
                print("Found END marker (waiting for Bias/Temp lines)…")
                continue

            found = BIN8.findall(line)
            if found:
                tokens.extend(found)
                last_new = time.time()
                if len(tokens) % 1000 == 0:
                    print(f"  {len(tokens)}/{TOTAL_BYTES} bytes…")

            # Once all tokens are in, DON'T break yet: the Arduino prints
            # "Bias:" and "Temp:" AFTER the PUF data block, in this order:
            #   END -> Bias -> COMPLETE -> LoRa -> Temp
            # so "Temp:" is the LAST thing. Wait for it specifically. Fall back
            # to a grace timeout if temperature never arrives (e.g. old firmware).
            if len(tokens) >= TOTAL_BYTES:
                tokens = tokens[:TOTAL_BYTES]
                if temp is not None:        # got the temperature -> done
                    break
                if "Temp:" in line:         # temp line seen this iteration
                    break
                if time.time() - last_new > 3.0:  # grace: no temp (old firmware)
                    print("  (no Temp line within grace window)")
                    break
                continue

        print(f"Total tokens collected: {len(tokens)}")
        return tokens[:TOTAL_BYTES], bias, temp

    # ----- transformations / saving -----
    def tokens_to_grid_text(self, tokens):
        """256 lines, each 32 comma-separated 8-bit binary strings + newline."""
        if len(tokens) < TOTAL_BYTES:
            tokens = tokens + ["00000000"] * (TOTAL_BYTES - len(tokens))
        else:
            tokens = tokens[:TOTAL_BYTES]
        lines = []
        for r in range(ROWS):
            row = tokens[r * COLS : (r + 1) * COLS]
            lines.append(",".join(row))
        return "\n".join(lines) + "\n"

    def tokens_to_hex_csv_rows(self, tokens):
        """Returns a list-of-lists (rows) for CSV: ['addr','b00'..'b31'] + 256 rows."""
        if len(tokens) < TOTAL_BYTES:
            tokens = tokens + ["00000000"] * (TOTAL_BYTES - len(tokens))
        else:
            tokens = tokens[:TOTAL_BYTES]
        ints = [int(t, 2) for t in tokens]
        rows = [["addr"] + [f"b{c:02d}" for c in range(COLS)]]
        for r in range(ROWS):
            base = r * COLS
            addr_hex = f"0x{base:04X}"
            row_hex = [f"0x{ints[base + c]:02X}" for c in range(COLS)]
            rows.append([addr_hex] + row_hex)
        return rows

    def save_capture(self, chip, text_grid, hex_rows, bias=None, temp=None):
        ts = datetime.now().strftime("PUF_%Y%m%d_%H%M%S_%f")[:-3]
        sub = f"SRAM{chip}"
        name = ts
        if bias is not None:
            name = f"{name}_bias{bias:.1f}"
        if temp is not None:
            name = f"{name}_temp{temp:.1f}"

        txt_path = os.path.join(OUT_BASE, sub, f"{name}.txt")
        with open(txt_path, "w", newline="\n") as f:
            f.write(text_grid)

        csv_path = os.path.join(OUT_BASE, sub, f"{name}.csv")
        with open(csv_path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerows(hex_rows)

        if bias is not None or temp is not None:
            self.stats[f"chip{chip}"].append(
                {"timestamp": ts, "bias": bias, "temp_c": temp,
                 "txt_file": txt_path, "csv_file": csv_path}
            )
        return txt_path, csv_path

    def save_stats(self):
        stats_path = os.path.join(OUT_BASE, "analysis", "capture_stats.json")
        with open(stats_path, "w") as f:
            json.dump(self.stats, f, indent=2)
        print(f"\nStats saved: {stats_path}")
        for chip in (1, 2):
            biases = [s["bias"] for s in self.stats[f"chip{chip}"] if s.get("bias") is not None]
            if biases:
                avg = sum(biases) / len(biases)
                print(f"SRAM{chip}: {len(biases)} caps, avg bias {avg:.2f}%, "
                      f"range {min(biases):.2f}–{max(biases):.2f}%")
            temps = [s["temp_c"] for s in self.stats[f"chip{chip}"] if s.get("temp_c") is not None]
            if temps:
                avg_t = sum(temps) / len(temps)
                print(f"SRAM{chip}: {len(temps)} temp readings, avg {avg_t:.2f} C, "
                      f"range {min(temps):.2f}–{max(temps):.2f} C")

    # ----- run modes -----
    def run_single(self, ser, chip, count):
        print(f"\n=== Capturing {count}× from SRAM{chip} ===")
        for i in range(count):
            print(f"[{i+1}/{count}] SRAM{chip}")
            self.send_cmd(ser, chip)
            tokens, bias, temp = self.read_tokens(ser)
            grid = self.tokens_to_grid_text(tokens)
            hex_rows = self.tokens_to_hex_csv_rows(tokens)
            txt_path, csv_path = self.save_capture(chip, grid, hex_rows, bias, temp)
            print(f"Saved {os.path.basename(txt_path)} / {os.path.basename(csv_path)}")
            time.sleep(LOOP_DELAY)

    def run_dual_alt(self, ser, per_chip):
        print(f"\n=== Alternating: {per_chip} capture(s) per chip ===")
        for i in range(per_chip):
            for chip in (1, 2):
                print(f"[{i+1}/{per_chip}] SRAM{chip}")
                self.send_cmd(ser, chip)
                tokens, bias, temp = self.read_tokens(ser)
                grid = self.tokens_to_grid_text(tokens)
                hex_rows = self.tokens_to_hex_csv_rows(tokens)
                txt_path, csv_path = self.save_capture(chip, grid, hex_rows, bias, temp)
                print(f"Saved {os.path.basename(txt_path)} / {os.path.basename(csv_path)}")
                time.sleep(LOOP_DELAY)

    def run_seq(self, ser, per_chip):
        print(f"\n=== Sequential: {per_chip} capture(s) per chip ===")
        for chip in (1, 2):
            self.run_single(ser, chip, per_chip)

    # ----- entrypoint -----
    def run(self):
        self.ensure_dirs()
        try:
            with self.open_serial() as ser:
                # Show status once
                ser.write(b"S\n"); time.sleep(0.5)  # show status once

                if MODE.upper() == "SINGLE":
                    self.run_single(ser, SINGLE_CHIP, CAPTURES_PER_CHIP)
                elif MODE.upper() == "DUAL_ALTERNATE":
                    self.run_dual_alt(ser, CAPTURES_PER_CHIP)
                elif MODE.upper() == "SEQUENTIAL":
                    self.run_seq(ser, CAPTURES_PER_CHIP)
                else:
                    print(f"Unknown MODE={MODE}")
                    return

                self.save_stats()

        except KeyboardInterrupt:
            print("\nStopped by user.")
        except Exception as e:
            print(f"\nError: {e}")

        print("\nCapture complete!")

if __name__ == "__main__":
    PUFLogger(PORT, BAUD).run()
