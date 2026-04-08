import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from sklearn.linear_model import LinearRegression

# Load the data
# Change the file path here to use whatever csv you are visualizing
file_path = 'Downloads/CheepSyncData.csv'
df = pd.read_csv(file_path)

# Extract relevant columns
seq = df['seq'].values.reshape(-1, 1)
t_ns = df['t_us'].values * 1000  # Convert local microseconds to nanoseconds
received_at_ns = df['received_at_ns'].values

# Get final model parameters from the last row
final_alpha = df['cheep_sync_alpha_ns'].iloc[-1]
final_beta = df['cheep_sync_beta'].iloc[-1]

# Calculate the model-based line: synced_at_ns = alpha + beta * local_time_ns
model_synced_ns = final_alpha + (final_beta * t_ns)

# Perform linear regression for the actual received_at_ns vs seq
model_lr = LinearRegression()
model_lr.fit(seq, received_at_ns)
received_pred = model_lr.predict(seq)

# Create a 2x2 grid of plots for different scales
fig, axes = plt.subplots(2, 2, figsize=(16, 12))
ranges = [10, 25, 50, 100]
axes = axes.flatten()

for i, max_seq in enumerate(ranges):
    ax = axes[i]
    
    # Filter data for the current range
    mask = df['seq'] <= max_seq
    df_sub = df[mask]
    
    if df_sub.empty:
        ax.set_title(f"Sequence Range: 0 to {max_seq} (No Data)")
        continue

    seq_sub = df_sub['seq']
    received_sub = df_sub['received_at_ns']
    model_sub = final_alpha + (final_beta * (df_sub['t_us'] * 1000))
    
    # Best fit for THIS specific range (for visual reference)
    lr_sub = LinearRegression().fit(seq_sub.values.reshape(-1, 1), received_sub)
    pred_sub = lr_sub.predict(seq_sub.values.reshape(-1, 1))

    # Plotting
    ax.scatter(seq_sub, received_sub, color='blue', alpha=0.5, label='Actual received_at_ns', s=20)
    ax.plot(seq_sub, pred_sub, color='red', linestyle='--', label='Local Best Fit')
    ax.plot(seq_sub, model_sub, color='green', linewidth=2, label='Final CheepSync Model')
    
    ax.set_title(f'Sequence Range: 0 to {max_seq}')
    ax.set_xlabel('Sequence Number (seq)')
    ax.set_ylabel('Time (ns)')
    ax.legend(fontsize='small')
    ax.grid(True, linestyle=':', alpha=0.6)
    ax.ticklabel_format(style='sci', axis='y', scilimits=(0,0))

plt.tight_layout()

# Save the plot
output_plot = 'cheepsync_multi_scale_comparison.png'
plt.savefig(output_plot)
print(f"Multi-scale visualization saved to {output_plot}")
print(f"Final Parameters used: Alpha={final_alpha}, Beta={final_beta}")
