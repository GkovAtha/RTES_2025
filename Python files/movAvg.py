import glob
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from datetime import datetime

symbols = [
    "BTC-USDT", "ADA-USDT", "ETH-USDT", "DOGE-USDT",
    "XRP-USDT", "SOL-USDT", "LTC-USDT", "BNB-USDT"
]

symbol_data = {sym: [] for sym in symbols}


#Read all *_movingAvg.log files and extract moving average data
log_files = glob.glob("*_movingAvg.log")
print("Number of files found:", len(log_files))

for filename in log_files:
    for sym in symbols:
        if sym in filename:
            with open(filename, "r") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    parts = line.split(',')
                    if len(parts) < 3:
                        print(f"Invalid line in {filename}, skipping: {line}")
                        continue
                    time_str = parts[0]
                    moving_avg_str = parts[1]
                    try:
                        ts = datetime.strptime(time_str, "%Y-%m-%d %H:%M:%S")
                        moving_avg = float(moving_avg_str)
                        symbol_data[sym].append((ts, moving_avg))
                    except Exception as e:
                        print(f"Error parsing line in {filename}: {line} ({e})")
            break

for sym, data in symbol_data.items():
    print(f"{sym}: {len(data)} data points")


#Plotting
fig, ax = plt.subplots(figsize=(12, 6))

for sym, data in symbol_data.items():
    if not data:
        continue
    data_sorted = sorted(data, key=lambda x: x[0])
    times, moving_avgs = zip(*data_sorted)
    base = moving_avgs[0]
    rel_changes = [(val - base) / base * 100 for val in moving_avgs]
    ax.plot(times, rel_changes, linestyle="-", label=sym)

ax.set_xlabel("Time", fontsize=14)
ax.set_ylabel("Relative Change", fontsize=14)
ax.set_title("Change in Moving Average", fontsize=16)

locator = mdates.AutoDateLocator()
formatter = mdates.DateFormatter("%Y-%m-%d\n%H:%M")
ax.xaxis.set_major_locator(locator)
ax.xaxis.set_major_formatter(formatter)
plt.setp(ax.get_xticklabels(), rotation=45, ha="right")

ax.grid(True, linestyle="--", alpha=0.7)
ax.legend(fontsize=12)
fig.tight_layout()
plt.show()