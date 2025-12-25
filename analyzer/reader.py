import struct
import os

# Configuration
# This must match the location where your C++ Consumer saves the file
LOG_FILE = "../src/telemetry.alog" 

def read_binary_logs(filename):
    if not os.path.exists(filename):
        print(f"Error: Could not find {filename}")
        print("Did you run the C++ Consumer? (It creates the file)")
        return

    file_size = os.path.getsize(filename)
    print(f"Found log file: {filename} ({file_size} bytes)")

    # --- The Struct Format ---
    # Q = unsigned long long (8 bytes) -> timestamp
    # i = integer (4 bytes)            -> event_id
    # d = double (8 bytes)             -> value
    # The compiler adds 4 bytes of padding after 'i' to align 'd'
    # So the total size is usually 24 bytes per entry.
    struct_fmt = 'Qid' 
    entry_size = struct.calcsize(struct_fmt) 

    print(f"Reading logs (Expect {entry_size} bytes per entry)...")

    with open(filename, "rb") as f:
        count = 0
        while True:
            # Read exactly one struct's worth of bytes
            chunk = f.read(entry_size)
            
            # If we hit the end of the file, stop
            if not chunk:
                break
            
            try:
                # Unpack the binary data into variables
                timestamp, event_id, value = struct.unpack(struct_fmt, chunk)
                
                # Print the first 5 and last 5 logs to verify
                if count < 5 or count >= 9995:
                    print(f"Row {count}: ID={event_id} | Value={value:.2f} | Time={timestamp}")
                
                count += 1
                
            except struct.error:
                print("Error: Corrupted or incomplete log entry.")
                break

    print(f"\n[Success] Analyzed {count} total logs.")

if __name__ == "__main__":
    read_binary_logs(LOG_FILE)