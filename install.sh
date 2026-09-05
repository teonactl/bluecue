#!/usr/bin/env bash
# install.sh — installa BlueCue per l'utente corrente (nessun sudo richiesto)
# Compatibile con: KDE Plasma, GNOME, Ubuntu (Unity/GNOME), XFCE, MATE,
#                  Cinnamon, LXQt, LXDE, Budgie e qualsiasi DE XDG-compliant.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Trova il binario compilato ────────────────────────────────────────────────
BINARY=""
for candidate in \
    "$SCRIPT_DIR/build/bluecue" \
    "$SCRIPT_DIR/build_asan/bluecue"
do
    if [[ -x "$candidate" ]]; then
        BINARY="$candidate"
        break
    fi
done

if [[ -z "$BINARY" ]]; then
    echo "Errore: nessun binario trovato. Compila prima con:"
    echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)"
    exit 1
fi

echo "Binario trovato: $BINARY"

# ── Destinazioni XDG (nessun sudo richiesto) ──────────────────────────────────
BIN_DIR="${HOME}/.local/bin"
ICONS_DIR="${HOME}/.local/share/icons/hicolor"
DESKTOP_DIR="${HOME}/.local/share/applications"

mkdir -p "$BIN_DIR" \
         "$ICONS_DIR/scalable/apps" \
         "$ICONS_DIR/256x256/apps" \
         "$ICONS_DIR/128x128/apps" \
         "$ICONS_DIR/64x64/apps" \
         "$ICONS_DIR/48x48/apps" \
         "$DESKTOP_DIR"

# ── Installa il binario ───────────────────────────────────────────────────────
cp -f "$BINARY" "$BIN_DIR/bluecue"
chmod +x "$BIN_DIR/bluecue"
echo "  [✓] Binario  → $BIN_DIR/bluecue"

# ── Installa icona SVG ────────────────────────────────────────────────────────
cp -f "$SCRIPT_DIR/res/icon.svg" "$ICONS_DIR/scalable/apps/bluecue.svg"
echo "  [✓] Icona SVG installata"

# ── Genera icone PNG rasterizzate ─────────────────────────────────────────────
# Necessario per KDE e alcuni temi GTK che non supportano SVG nell'hicolor cache.
_svg="$ICONS_DIR/scalable/apps/bluecue.svg"
_converter=""
if   command -v rsvg-convert &>/dev/null; then _converter="rsvg"
elif command -v inkscape      &>/dev/null; then _converter="inkscape"
elif command -v convert       &>/dev/null; then _converter="magick"
fi

if [[ -n "$_converter" ]]; then
    for size in 256 128 64 48; do
        _out="$ICONS_DIR/${size}x${size}/apps/bluecue.png"
        case "$_converter" in
            rsvg)    rsvg-convert -w "$size" -h "$size" "$_svg" -o "$_out" ;;
            inkscape) inkscape --export-type=png --export-width="$size" \
                               --export-filename="$_out" "$_svg" 2>/dev/null ;;
            magick)  convert -background none -resize "${size}x${size}" "$_svg" "$_out" ;;
        esac
    done
    echo "  [✓] Icone PNG generate (48, 64, 128, 256 px) via $_converter"
else
    echo "  [!] PNG non generati: installa rsvg-convert (librsvg), inkscape o imagemagick"
    echo "      sudo pacman -S librsvg   # Arch"
    echo "      sudo apt install librsvg2-bin  # Debian/Ubuntu"
fi

# ── Installa il file .desktop ─────────────────────────────────────────────────
sed "s|Exec=bluecue|Exec=$BIN_DIR/bluecue|" \
    "$SCRIPT_DIR/res/bluecue.desktop" \
    > "$DESKTOP_DIR/bluecue.desktop"
chmod 644 "$DESKTOP_DIR/bluecue.desktop"
echo "  [✓] Launcher → $DESKTOP_DIR/bluecue.desktop"

# ── Aggiorna le cache (tutti i DE) ────────────────────────────────────────────
echo ""
echo "Aggiornamento cache..."

# Standard XDG (funziona per tutti i DE)
command -v update-desktop-database &>/dev/null && \
    update-desktop-database "$DESKTOP_DIR" 2>/dev/null && \
    echo "  [✓] update-desktop-database" || true

# KDE Plasma 5 / 6 — ricostruisce l'indice dei servizi per il menu start
if command -v kbuildsycoca6 &>/dev/null; then
    kbuildsycoca6 --noincremental 2>/dev/null && echo "  [✓] kbuildsycoca6 (KDE)"
elif command -v kbuildsycoca5 &>/dev/null; then
    kbuildsycoca5 --noincremental 2>/dev/null && echo "  [✓] kbuildsycoca5 (KDE)"
fi

# GNOME Shell — notifica il compositor tramite D-Bus (nessun riavvio)
if command -v gdbus &>/dev/null && [[ "${XDG_CURRENT_DESKTOP:-}" == *"GNOME"* ]]; then
    gdbus call --session \
          --dest org.gnome.Shell \
          --object-path /org/gnome/Shell \
          --method org.gnome.Shell.Eval \
          'global.reexec_self()' 2>/dev/null || true
    # Fallback: aggiorna solo la lista app senza riavviare la shell
    gdbus call --session \
          --dest org.gnome.Shell \
          --object-path /org/gnome/Shell \
          --method org.gnome.Shell.Eval \
          '' 2>/dev/null || true
fi

# GTK icon cache (GNOME, XFCE, MATE, Cinnamon, Budgie, LXQt)
if command -v gtk-update-icon-cache &>/dev/null; then
    gtk-update-icon-cache -f -t "$ICONS_DIR" 2>/dev/null && \
        echo "  [✓] gtk-update-icon-cache" || true
fi

# XFCE — ricarica il pannello se in esecuzione
if [[ "${XDG_CURRENT_DESKTOP:-}" == *"XFCE"* ]]; then
    command -v xfce4-panel &>/dev/null && \
        xfce4-panel --restart 2>/dev/null &
fi

# ── Risultato ─────────────────────────────────────────────────────────────────
echo ""
echo "Installazione completata."
echo "BlueCue dovrebbe ora apparire nel menu applicazioni."
echo "Se non appare subito, fai logout e login per forzare il refresh del menu."

# ── Avvisa se ~/.local/bin non è nel PATH ─────────────────────────────────────
if [[ ":$PATH:" != *":$BIN_DIR:"* ]]; then
    echo ""
    echo "NOTA: $BIN_DIR non è nel tuo PATH."
    echo "Aggiungi questa riga al tuo ~/.bashrc o ~/.zshrc:"
    echo "  export PATH=\"\$HOME/.local/bin:\$PATH\""
fi
