import matplotlib.pyplot as plt
from matplotlib.dates import DateFormatter, AutoDateLocator
from datetime import datetime
import matplotlib.dates as mdates
import numpy as np
from scipy.interpolate import make_interp_spline

timestamps = []
cpu_idle_percentages = []

#Open and read the cpu_idle.log file
with open("cpu_idle.log", "r") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            time_str, idle_str = line.split(',')
            ts = datetime.strptime(time_str, "%Y-%m-%d %H:%M:%S")
            idle = float(idle_str)
            timestamps.append(ts)
            cpu_idle_percentages.append(idle)
        except Exception as e:
            print("Could not parse line:", line, e)

#Calculate average CPU idle percentage
average_idle = np.mean(cpu_idle_percentages)

#Convert datetime to numerical format for interpolation
x = mdates.date2num(timestamps)
y = np.array(cpu_idle_percentages)

sorted_idx = np.argsort(x)
x = x[sorted_idx]
y = y[sorted_idx]

num_points = 300
x_new = np.linspace(x.min(), x.max(), num_points)
spline = make_interp_spline(x, y, k=3)
y_smooth = spline(x_new)
dates_smooth = mdates.num2date(x_new)

#Create the plot
plt.figure(figsize=(12, 6))
plt.plot(dates_smooth, y_smooth, linestyle="-", color="blue", label="CPU Idle %")
plt.axhline(y=average_idle, color="red", linestyle="--", label=f"Average: {average_idle:.2f}%")
plt.xlabel("Time", fontsize=14)
plt.ylabel("CPU Idle (%)", fontsize=14)
plt.title("CPU Idle Percentage Over Time", fontsize=16)
ax = plt.gca()
locator = AutoDateLocator()
formatter = DateFormatter("%Y-%m-%d\n%H:%M")
ax.xaxis.set_major_locator(locator)
ax.xaxis.set_major_formatter(formatter)

plt.xticks(rotation=45)
plt.grid(True, linestyle="--", alpha=0.7)
plt.legend(fontsize=12)
plt.tight_layout()
plt.show()