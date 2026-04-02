import soundfile as sf
import pyloudnorm as pyln
import numpy as np
from scipy.signal import resample_poly
import json
import os
from glob import glob

# === Einstellungen ===
input_folder = r"C:\Users\flori\Documents\Studium\Module\HSD\SigVProjekt\Referenzsongs\pop"       # Ordner mit WAV-Dateien
json_template = "test.JSON"        # Vorlage der Referenzstruktur
output_json = "pop_medref.json"  # Ausgabe-Datei

target_lufs = -14.0
true_peak_limit = -1.0
oversample_factor = 2

# === Hilfsfunktionen ===

def oversample(sig, factor):
    if sig.ndim == 1:
        return resample_poly(sig, up=factor, down=1)
    return np.stack([resample_poly(sig[:, ch], up=factor, down=1)
                     for ch in range(sig.shape[1])], axis=1)


def fast_limiter(sig, limit_db=-1.0):
    limit = 10 ** (limit_db / 20)
    if sig.ndim == 1:
        max_val = np.max(np.abs(sig))
        if max_val > limit:
            sig = sig * (limit / max_val)
        return sig
    else:
        max_vals = np.max(np.abs(sig), axis=0)
        scaling = np.ones_like(max_vals)
        over = max_vals > limit
        scaling[over] = limit / max_vals[over]
        return sig * scaling


def compute_band_rms_db(freqs, lin_mag, center):
    # 1/3-Oktavbereich
    lower = center / (2 ** (1/6))
    upper = center * (2 ** (1/6))
    mask = (freqs >= lower) & (freqs <= upper)

    if not np.any(mask):
        return np.nan

    # RMS im linearen Bereich (Amplitude² → Energie)
    rms = np.sqrt(np.mean(lin_mag[mask]**2))
    return 20 * np.log10(rms + 1e-12)   # dBFS


def analyze_file(path, meter, template):
    data, sr = sf.read(path)

    # Mono für FFT
    if data.ndim > 1:
        data = np.mean(data, axis=1)

    # Lautheit messen
    loudness = meter.integrated_loudness(data)

    # Normalisieren auf -14 LUFS
    data = pyln.normalize.loudness(data, loudness, target_lufs)

    # True Peak check + Limiter
    data_ov = oversample(data, oversample_factor)
    peak_db = 20 * np.log10(np.max(np.abs(data_ov)) + 1e-12)
    if peak_db > true_peak_limit:
        data = fast_limiter(data, true_peak_limit)

    # ===== FFT richtig normiert =====
    N = len(data)
    window = np.hanning(N)
    data_win = data * window

    fft_spectrum = np.fft.rfft(data_win)
    freqs = np.fft.rfftfreq(N, 1 / sr)

    # Lineare Amplitude korrekt normiert (dBFS)
    lin_mag = np.abs(fft_spectrum) / (N / 2)

    # ===== Terzbänder =====
    band_values = []
    for band in template["bands"]:
        f_center = band["freq"]
        band_db = compute_band_rms_db(freqs, lin_mag, f_center)
        band_values.append(band_db)

    return band_values


# ==========================
# Hauptprozess
# ==========================
files = glob(os.path.join(input_folder, "*.wav"))
if not files:
    raise FileNotFoundError("Keine WAV-Dateien gefunden.")

print(f"{len(files)} Dateien gefunden. Starte Analyse…")

# Vorlage laden
with open(json_template, "r", encoding="utf-8") as f:
    template = json.load(f)

# Samplerate für Meter bestimmen
data_ref, sr_ref = sf.read(files[0])
if data_ref.ndim > 1:
    sr_meter = sr_ref
else:
    sr_meter = sr_ref

meter = pyln.Meter(sr_meter, block_size=0.400, filter_class="K-weighting")

all_band_values = []

for i, path in enumerate(files, 1):
    print(f"[{i}/{len(files)}] {os.path.basename(path)}")
    try:
        vals = analyze_file(path, meter, template)
        all_band_values.append(vals)
    except Exception as e:
        print(f"Fehler in {path}: {e}")

all_band_values = np.array(all_band_values)

# ==========================
# p10, median, p90 berechnen
# ==========================
p10 = np.nanpercentile(all_band_values, 10, axis=0)
median = np.nanmedian(all_band_values, axis=0)
p90 = np.nanpercentile(all_band_values, 90, axis=0)

# ==========================
# JSON bauen
# ==========================
bands_out = []
for i, band in enumerate(template["bands"]):
    bands_out.append({
        "freq": band["freq"],
        "p10": float(p10[i]),
        "median": float(median[i]),
        "p90": float(p90[i])
    })

result = {
    "name": template["name"],
    "description": f"Genre-Referenzkurve aus {len(files)} Songs (−14 LUFS normalisiert)",
    "bands": bands_out
}

with open(output_json, "w", encoding="utf-8") as f:
    json.dump(result, f, indent=2, ensure_ascii=False)

print(f"\nFERTIG! JSON gespeichert als: {output_json}")
