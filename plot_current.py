"""
POWER-Z KM003C - Stromverlauf plotten
Liest 'user data.csv' ein und erzeugt ein matplotlib-Diagramm des IBUS-Stromverlaufs
inkl. geglaettetem Verlauf (gleitender Mittelwert).

Aufruf:
    python plot_current.py                          # Standarddatei, Fenster=151 Samples
    python plot_current.py "pfad/zur/datei.csv"     # andere Datei
    python plot_current.py "datei.csv" 301          # andere Datei + Fensterbreite

CSV-Spalten: ElapsedTime, Unix, VBUS, IBUS, DP, DM, CC1, CC2, TEMP, CHARGE, ENERGY, PWR
"""

import sys
import os
import numpy as np
import matplotlib.pyplot as plt

# Fensterbreite des Glaettungsfilters in Samples (bei 1 kHz ~ Millisekunden).
# Groesser = glatter, aber traeger. Ungerade Zahl empfohlen.
# Per Kommandozeile ueberschreibbar:  python plot_current.py "datei.csv" 1001
SMOOTH_WINDOW = 501


def moving_average(y, window):
    """Zentrierter gleitender Mittelwert mit reflektierender Randbehandlung.

    Vermeidet die typischen Einschwing-Artefakte an Anfang/Ende, die bei
    einfacher Faltung entstehen. Fensterbreite wird auf ungerade normiert.
    """
    window = int(window)
    if window < 2:
        return y.copy()
    if window % 2 == 0:
        window += 1
    pad = window // 2
    kernel = np.ones(window, dtype=float) / window
    y_padded = np.pad(y, pad, mode="reflect")
    return np.convolve(y_padded, kernel, mode="valid")

# --- Datei bestimmen ---------------------------------------------------------
DEFAULT_CSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "user data.csv")
csv_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_CSV
window = int(sys.argv[2]) if len(sys.argv) > 2 else SMOOTH_WINDOW

if not os.path.isfile(csv_path):
    sys.exit(f"Datei nicht gefunden: {csv_path}")

# --- Einlesen ----------------------------------------------------------------
# Spalte 1 = Zeit in Sekunden (Header 'Unix', hier relative Sekunden ab 0)
# Spalte 3 = IBUS in Ampere
# usecols vermeidet Probleme mit der teilweise leeren TEMP-Spalte.
print(f"Lese: {csv_path}")
data = np.genfromtxt(
    csv_path,
    delimiter=",",
    skip_header=1,
    usecols=(1, 3),
    dtype=float,
)

t = data[:, 0]              # Zeit [s]
i_a = data[:, 1]           # Strom [A]
i_ma = i_a * 1000.0        # Strom [mA] (angenehmere Achse)

# --- Glaettung ---------------------------------------------------------------
i_smooth = moving_average(i_ma, window)

# --- Kennwerte ---------------------------------------------------------------
i_avg, i_min, i_max = i_ma.mean(), i_ma.min(), i_ma.max()
dur = t[-1] - t[0]
# Tatsaechlich entferntes Rauschen = Streuung der Differenz (roh - geglaettet).
# Das isoliert das Hochfrequenz-Rauschen von den echten Laststufen.
noise_removed = (i_ma - i_smooth).std()
print(f"Datenpunkte: {len(t)}   Dauer: {dur:.3f} s")
print(f"Strom [mA]:  Ø {i_avg:.2f}   min {i_min:.2f}   max {i_max:.2f}")
print(f"Glaettung:   Fenster {window} Samples (~{window} ms bei 1 kHz)")
print(f"Entferntes Rauschen: sigma {noise_removed:.2f} mA")

# --- Plot --------------------------------------------------------------------
fig, ax = plt.subplots(figsize=(12, 5))

# Rohsignal dezent im Hintergrund, geglaetteter Verlauf betont darueber
ax.plot(t, i_ma, color="#f4a3a3", linewidth=0.6, label="IBUS (roh)", zorder=1)
ax.plot(t, i_smooth, color="#b3121b", linewidth=1.6,
        label=f"IBUS geglaettet ({window} Samples)", zorder=3)
ax.axhline(i_avg, color="#1f77b4", linestyle="--", linewidth=1.0,
           label=f"Ø {i_avg:.2f} mA", zorder=2)

ax.set_title("POWER-Z KM003C - Stromverlauf (IBUS)")
ax.set_xlabel("Zeit [s]")
ax.set_ylabel("Strom [mA]")
ax.grid(True, which="both", linestyle=":", alpha=0.6)
ax.margins(x=0)
ax.legend(loc="upper right")

# Kennwerte als Textbox
info = (f"Ø {i_avg:.2f} mA\n"
        f"min {i_min:.2f} mA\n"
        f"max {i_max:.2f} mA\n"
        f"Dauer {dur:.1f} s\n"
        f"entf. Rauschen sigma {noise_removed:.2f} mA")
ax.text(0.01, 0.97, info, transform=ax.transAxes, va="top", ha="left",
        fontsize=9, family="monospace",
        bbox=dict(boxstyle="round", facecolor="white", alpha=0.8))

fig.tight_layout()

# --- Geglaetteten Verlauf als CSV exportieren --------------------------------
# Fuer die Weiteranalyse (Sockelstrom des Boards). Spalten:
#   time_s        - Zeit [s]
#   i_raw_mA      - Rohstrom [mA]
#   i_smooth_mA   - geglaetteter Strom [mA] (gleitender Mittelwert)
out_csv = os.path.join(os.path.dirname(csv_path), "stromverlauf_smooth.csv")
np.savetxt(
    out_csv,
    np.column_stack((t, i_ma, i_smooth)),
    delimiter=",",
    header="time_s,i_raw_mA,i_smooth_mA",
    comments="",
    fmt="%.6f",
)
print(f"Geglaetteter Verlauf exportiert: {out_csv}")

# --- Speichern + Anzeigen ----------------------------------------------------
out_png = os.path.join(os.path.dirname(csv_path), "stromverlauf.png")
fig.savefig(out_png, dpi=150)
print(f"Diagramm gespeichert: {out_png}")

plt.show()
