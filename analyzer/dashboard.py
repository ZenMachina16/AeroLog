import struct
import matplotlib.pyplot as plt
import datetime
import os

LOG_FILE = "../src/telemetry.alog"

def read_data():
    prices = []
    timestamps = []
    
    # 1. Read the Binary File
    if not os.path.exists(LOG_FILE):
        print("Error: No log file found!")
        return [], []

    struct_fmt = 'Qid' # Timestamp (8), ID (4), Value (8)
    entry_size = struct.calcsize(struct_fmt)
    
    print("Reading binary data...")
    with open(LOG_FILE, "rb") as f:
        while True:
            chunk = f.read(entry_size)
            if not chunk: break
            
            try:
                ts, _, price = struct.unpack(struct_fmt, chunk)
                
                # Convert Unix Timestamp to Date Object for plotting
                dt_object = datetime.datetime.fromtimestamp(ts)
                
                timestamps.append(dt_object)
                prices.append(price)
            except struct.error:
                break
                
    return timestamps, prices

def plot_chart(timestamps, prices):
    if not prices:
        print("No data to plot.")
        return

    print(f"Plotting {len(prices)} data points...")
    
    plt.figure(figsize=(12, 6))
    plt.plot(timestamps, prices, color='#00ff00', linewidth=0.5, label='Price')
    
    # Styling to make it look like a Trading Terminal
    plt.title("AeroLog Reconstruction: High-Frequency Market Data", fontsize=14)
    plt.xlabel("Time", fontsize=10)
    plt.ylabel("Price (USD)", fontsize=10)
    plt.grid(True, linestyle='--', alpha=0.3)
    plt.legend()
    plt.tight_layout()
    
    # Save the graph
    output_file = "market_replay.png"
    plt.savefig(output_file, dpi=100)
    print(f"Graph saved to {output_file}")
    plt.show()

if __name__ == "__main__":
    t, p = read_data()
    plot_chart(t, p)