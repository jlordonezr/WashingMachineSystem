# Manuals

| PDF | Source | For |
|---|---|---|
| `ReuseWater_Runbook.pdf` | `src/Runbook.html` | Installers, operators and maintenance |
| `ReuseWater_Quick_Card.pdf` | `src/QuickCard.html` | The household (pages 1–2) and the installer's commissioning checklist |

Every firmware, wiring or app change must be reflected in both documents. Edit the HTML sources (shared styles in `src/manual.css`, diagrams in `src/img/`; the board pinout and the buzzer wiring panel are inline SVG in `Runbook.html`), add a row to the Runbook's revision history, then rebuild the PDFs:

```
sh manuals/build.sh
```

The build uses headless Google Chrome (US Letter, page numbers and running titles come from the `@page` rules). Commit the sources and the PDFs together.

Until version 3.1 the manuals were named `GreyWater_Runbook.pdf` and `GreyWater_Quick_Card.pdf` and existed only as PDFs; version 3.2 rebuilt them from these sources.
