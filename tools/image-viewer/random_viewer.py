#!/usr/bin/env python3
"""
Random image viewer: picks a random image from 3 folders and displays it for N seconds.

Now:
- Portable / scalable window (resizable)
- Image auto-fits on every resize (keeps aspect ratio)
- Sensible minimum window size
- Optional: --fullscreen

Keys:
  Space  = next image
  Esc/q  = quit

Requires:
  pip install pillow
"""

import argparse
import random
import sys
import tkinter as tk
from pathlib import Path
from PIL import Image, ImageTk


SUPPORTED_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"}


def collect_images(folder: Path) -> list[Path]:
    if not folder.exists() or not folder.is_dir():
        return []
    files: list[Path] = []
    for p in folder.rglob("*"):
        if p.is_file() and p.suffix.lower() in SUPPORTED_EXTS:
            files.append(p)
    return files


def fit_image_to_window(img: Image.Image, max_w: int, max_h: int) -> Image.Image:
    w, h = img.size
    if w <= 0 or h <= 0:
        return img

    scale = min(max_w / w, max_h / h)
    new_w = max(1, int(w * scale))
    new_h = max(1, int(h * scale))
    return img.resize((new_w, new_h), Image.LANCZOS)


class RandomImageViewer:
    def __init__(self, root: tk.Tk, image_paths: list[Path], seconds: float, shuffle_mode: str):
        self.root = root
        self.image_paths = image_paths
        self.seconds_ms = int(seconds * 1000)
        self.shuffle_mode = shuffle_mode

        # Window behavior: portable/scalable
        self.root.title("Random Image Viewer")
        self.root.configure(bg="black")
        self.root.resizable(True, True)
        self.root.minsize(640, 480)

        # Make the grid expand so the label stretches
        self.root.grid_rowconfigure(0, weight=1)
        self.root.grid_columnconfigure(0, weight=1)

        # UI
        self.label = tk.Label(self.root, bg="black")
        self.label.grid(row=0, column=0, sticky="nsew")

        self.info = tk.Label(
            self.root,
            text="",
            fg="white",
            bg="black",
            anchor="w",
            padx=10,
            pady=5,
            font=("Segoe UI", 10),
        )
        self.info.grid(row=1, column=0, sticky="ew")

        # Keep references
        self._photo = None
        self._after_id = None
        self._current_path: Path | None = None
        self._current_img: Image.Image | None = None  # cache decoded image to resize smoothly
        self._resize_after_id = None

        # Key binds
        self.root.bind("<Escape>", lambda e: self.close())
        self.root.bind("q", lambda e: self.close())
        self.root.bind("<space>", lambda e: self.next_image())  # skip

        # Resize handling (debounced to avoid heavy CPU during drag-resize)
        self.root.bind("<Configure>", self._on_resize)

        # Start with a reasonable window size (portable across screens)
        self._set_initial_geometry()

        # Initial
        self.next_image()

    def _set_initial_geometry(self):
        # 80% of the screen size, centered
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        w = int(sw * 0.80)
        h = int(sh * 0.80)
        x = max(0, (sw - w) // 2)
        y = max(0, (sh - h) // 2)
        self.root.geometry(f"{w}x{h}+{x}+{y}")

    def close(self):
        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
        if self._resize_after_id is not None:
            self.root.after_cancel(self._resize_after_id)
        self.root.destroy()

    def _pick_random(self) -> Path:
        if self.shuffle_mode == "uniform":
            return random.choice(self.image_paths)

        # "balanced": randomly pick a folder first, then pick an image from that folder
        by_folder: dict[Path, list[Path]] = {}
        for p in self.image_paths:
            by_folder.setdefault(p.parent, []).append(p)
        folder = random.choice(list(by_folder.keys()))
        return random.choice(by_folder[folder])

    def next_image(self):
        if not self.image_paths:
            self.info.config(text="No images found.")
            return

        self._current_path = self._pick_random()
        self._load_current_image()
        self._render_current()

        if self._after_id is not None:
            self.root.after_cancel(self._after_id)
        self._after_id = self.root.after(self.seconds_ms, self.next_image)

    def _load_current_image(self):
        self._current_img = None
        if self._current_path is None:
            return
        try:
            img = Image.open(self._current_path)
            self._current_img = img.convert("RGB")
            self.info.config(text=str(self._current_path))
        except Exception as e:
            self.info.config(text=f"Failed to open {self._current_path}: {e}")

    def _render_current(self):
        if self._current_img is None:
            return

        # Window size + subtract info bar height
        w = max(1, self.root.winfo_width())
        h = max(1, self.root.winfo_height() - self.info.winfo_height())
        if h <= 0:
            h = max(1, self.root.winfo_height() - 40)

        fitted = fit_image_to_window(self._current_img, w, h)
        self._photo = ImageTk.PhotoImage(fitted)
        self.label.configure(image=self._photo)

    def _on_resize(self, _event):
        # Debounce resize rendering to keep it smooth/portable across machines
        if self._resize_after_id is not None:
            self.root.after_cancel(self._resize_after_id)
        self._resize_after_id = self.root.after(60, self._render_current)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", type=str, default="../images", help="Base folder containing the 3 subfolders")
    ap.add_argument("--seconds", type=float, default=5.0, help="Seconds per image")
    ap.add_argument(
        "--folders",
        nargs="*",
        default=["ApisMellifera", "VespaCabro", "VespaVelutina"],
        help="Subfolder names inside --base",
    )
    ap.add_argument(
        "--shuffle",
        choices=["uniform", "balanced"],
        default="uniform",
        help="uniform = every image equal chance; balanced = each folder equal chance",
    )
    ap.add_argument("--fullscreen", action="store_true", help="Start fullscreen (Esc to exit)")
    args = ap.parse_args()

    base = Path(args.base).resolve()
    folders = [base / name for name in args.folders]

    all_images: list[Path] = []
    for f in folders:
        imgs = collect_images(f)
        print(f"[scan] {f} -> {len(imgs)} images")
        all_images.extend(imgs)

    if not all_images:
        print("No images found. Check your --base path and folder names.", file=sys.stderr)
        sys.exit(1)

    root = tk.Tk()

    viewer = RandomImageViewer(root, all_images, args.seconds, args.shuffle)

    if args.fullscreen:
        root.attributes("-fullscreen", True)

    root.mainloop()


if __name__ == "__main__":
    main()