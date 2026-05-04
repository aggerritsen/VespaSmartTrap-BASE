# Image Viewer Tool

Random desktop image viewer for quick visual checks of datasets and field captures.

By default it scans `tools/image-viewer/images`.

```powershell
pip install -r requirements.txt
.\run.ps1
```

Custom base folder:

```powershell
python .\random_viewer.py --base C:\path\to\images --seconds 5
```
