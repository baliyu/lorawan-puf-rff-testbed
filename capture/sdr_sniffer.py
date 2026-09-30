# sdr_sniffer.py
import os
import time
import numpy as np
from rtlsdr import RtlSdr
from datetime import datetime

# === CONFIGURATION ===
CENTER_FREQ   = 868100000  # 868.1 MHz
SAMPLE_RATE   = 2048000    # 2.048 MSps
GAIN          = 40         # SDR Gain
THRESHOLD     = 1.2        # Adjust this if it triggers too much/little
CAPTURE_LEN   = 256*1024   # ~128ms of data
OUTPUT_DIR    = "RFF_Data"

def main():
    if not os.path.exists(OUTPUT_DIR):
        os.makedirs(OUTPUT_DIR)

    sdr = RtlSdr()
    sdr.sample_rate = SAMPLE_RATE
    sdr.center_freq = CENTER_FREQ
    sdr.gain = GAIN
    
    print(f"--- RFFI SNIFFER ---")
    print(f"Freq: {CENTER_FREQ/1e6} MHz | Threshold: {THRESHOLD}")
    print("Waiting for Feather transmission...")
    
    # Dump initial samples to clear buffer
    sdr.read_samples(4096)
    
    pkt_count = 1

    try:
        while True:
            # 1. Listen (Fast Scan)
            samples = sdr.read_samples(4096)
            peak = np.max(np.abs(samples))
            
            # 2. Detect Packet
            if peak > THRESHOLD:
                print(f"[!] Energy Detected ({peak:.2f}) - Capturing Packet {pkt_count}...")
                
                # 3. Capture the full burst
                # We read a large chunk immediately to catch the packet
                data = sdr.read_samples(CAPTURE_LEN)
                
                # 4. Save to Disk
                ts = datetime.now().strftime("%H%M%S")
                fname = f"{OUTPUT_DIR}/pkt_{pkt_count:03d}_{ts}.npy"
                np.save(fname, data)
                print(f"    Saved: {fname}")
                
                pkt_count += 1
                time.sleep(0.5) # Wait for transmission to finish
                print("Listening...")

    except KeyboardInterrupt:
        print("\nStopping...")
        sdr.close()

if __name__ == "__main__":
    main()