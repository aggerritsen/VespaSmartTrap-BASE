# T-SIM Azure Downloader

Downloads T-SIM uploads from Azure Blob Storage using AzCopy and the local
firmware secrets in `t-sim/firmware/src/config_secrets.h`.

By default it downloads the active firmware prefixes:

- `photos`
- `logs`

Already downloaded files are not fetched again unless the Azure blob has a newer
timestamp. This is handled by AzCopy with `--overwrite=ifSourceNewer`.

## Usage

From the repo root:

```powershell
python .\tools\azure_download\download_azure_blobs.py
```

Dry run:

```powershell
python .\tools\azure_download\download_azure_blobs.py --dry-run
```

Download only one prefix:

```powershell
python .\tools\azure_download\download_azure_blobs.py --prefix photos
```

Download everything in the container:

```powershell
python .\tools\azure_download\download_azure_blobs.py --all
```

The default local output folder is `tools/azure_download/downloads/`.

## Analyze

Summarize downloaded logs and photos:

```powershell
python .\tools\azure_download\analyze_downloads.py
```

Plot confidence over time with simulated detection events:

```powershell
python .\tools\azure_download\plot_detection_timeline.py --threshold 0.745 --occurrence 2 --window-seconds 5
```
