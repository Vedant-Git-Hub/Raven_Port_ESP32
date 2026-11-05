#!/usr/bin/env python3
"""Convert images into the Elecrow Raven Gallery's 800x480 24-bit BMP format.

Run without arguments for the desktop converter screen, or pass --input and
--output to use the same converter from a terminal or automation script.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageOps, ImageTk


DISPLAY_SIZE = (800, 480)
PREVIEW_SIZE = (500, 300)
BACKGROUND = (11, 16, 32)
ROTATION_OPTIONS = {
    "Automatic / 0°": 0,
    "90° clockwise": 90,
    "180°": 180,
    "90° counter-clockwise": 270,
}


def flatten_to_rgb(image: Image.Image) -> Image.Image:
    """Apply orientation and flatten transparency onto the gallery background."""
    image = ImageOps.exif_transpose(image)
    if image.mode in ("RGBA", "LA") or (image.mode == "P" and "transparency" in image.info):
        canvas = Image.new("RGB", image.size, BACKGROUND)
        canvas.paste(image.convert("RGBA"), mask=image.convert("RGBA").getchannel("A"))
        return canvas
    return image.convert("RGB")


def prepare_image(source: Path, resize_mode: str, rotation: int = 0) -> Image.Image:
    """Open *source* and return an exact 800x480 RGB image.

    ``crop`` fills the display while preserving aspect ratio and center-crops
    excess content. ``fit`` letterboxes onto the gallery's dark background.
    ``rotation`` is applied clockwise after automatic EXIF orientation.
    """
    with Image.open(source) as opened:
        image = flatten_to_rgb(opened)
        if rotation:
            image = image.rotate(-rotation, expand=True)
        if resize_mode == "fit":
            fitted = ImageOps.contain(image, DISPLAY_SIZE, Image.Resampling.LANCZOS)
            result = Image.new("RGB", DISPLAY_SIZE, BACKGROUND)
            offset = ((DISPLAY_SIZE[0] - fitted.width) // 2,
                      (DISPLAY_SIZE[1] - fitted.height) // 2)
            result.paste(fitted, offset)
            return result
        return ImageOps.fit(image, DISPLAY_SIZE, Image.Resampling.LANCZOS,
                            centering=(0.5, 0.5))


def convert_image(source: Path, destination: Path, resize_mode: str, rotation: int = 0) -> None:
    """Write *source* as an uncompressed 24-bit 800x480 BMP at *destination*."""
    if not source.is_file():
        raise FileNotFoundError(f"Input image not found: {source}")
    if destination.suffix.lower() != ".bmp":
        destination = destination.with_suffix(".bmp")
    destination.parent.mkdir(parents=True, exist_ok=True)
    prepared = prepare_image(source, resize_mode, rotation)
    prepared.save(destination, format="BMP")


class ConverterApp(ttk.Frame):
    """Small desktop interface for the HMI image converter."""

    def __init__(self, master: tk.Tk) -> None:
        super().__init__(master, padding=18)
        self.master = master
        self.source = tk.StringVar()
        self.destination = tk.StringVar()
        self.resize_mode = tk.StringVar(value="crop")
        self.rotation = tk.StringVar(value="Automatic / 0°")
        self.status = tk.StringVar(value="Choose an image to prepare a display preview.")
        self.preview_image: ImageTk.PhotoImage | None = None
        self._build()

    def _build(self) -> None:
        self.grid(sticky="nsew")
        self.columnconfigure(1, weight=1)
        self.rowconfigure(5, weight=1)
        self.master.columnconfigure(0, weight=1)
        self.master.rowconfigure(0, weight=1)

        ttk.Label(self, text="HMI Gallery Image Converter", style="Title.TLabel").grid(
            row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(self, text="Exports uncompressed 24-bit BMP • 800 × 480 • Raven SD Gallery ready").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(2, 16))

        self._path_row(2, "Source image", self.source, self.choose_source, "Browse…")
        self._path_row(3, "Save BMP as", self.destination, self.choose_destination, "Save as…")

        options = ttk.LabelFrame(self, text="Resize behavior", padding=10)
        options.grid(row=4, column=0, columnspan=3, sticky="ew", pady=(12, 12))
        ttk.Radiobutton(options, text="Fill display (center crop)", value="crop",
                        variable=self.resize_mode, command=self.update_preview).grid(row=0, column=0, sticky="w")
        ttk.Radiobutton(options, text="Fit entire image (dark letterbox)", value="fit",
                        variable=self.resize_mode, command=self.update_preview).grid(row=1, column=0, sticky="w", pady=(5, 0))
        ttk.Label(options, text="Orientation").grid(row=0, column=1, sticky="w", padx=(28, 8))
        orientation = ttk.Combobox(options, state="readonly", width=24,
                                   textvariable=self.rotation,
                                   values=tuple(ROTATION_OPTIONS))
        orientation.grid(row=1, column=1, sticky="w", padx=(28, 0), pady=(5, 0))
        orientation.bind("<<ComboboxSelected>>", lambda _event: self.update_preview())

        preview = ttk.LabelFrame(self, text="800 × 480 output preview", padding=8)
        preview.grid(row=5, column=0, columnspan=3, sticky="nsew")
        preview.columnconfigure(0, weight=1)
        preview.rowconfigure(0, weight=1)
        self.preview_label = ttk.Label(preview, anchor="center", text="No image selected")
        self.preview_label.grid(row=0, column=0, sticky="nsew")

        ttk.Label(self, textvariable=self.status, wraplength=650).grid(
            row=6, column=0, columnspan=2, sticky="w", pady=(12, 0))
        ttk.Button(self, text="Convert to BMP", command=self.convert).grid(
            row=6, column=2, sticky="e", pady=(12, 0))

    def _path_row(self, row: int, label: str, variable: tk.StringVar, command, button: str) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=(0, 10), pady=4)
        entry = ttk.Entry(self, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", pady=4)
        entry.bind("<FocusOut>", lambda _event: self.update_preview())
        ttk.Button(self, text=button, command=command).grid(row=row, column=2, sticky="e", padx=(8, 0), pady=4)

    def choose_source(self) -> None:
        selected = filedialog.askopenfilename(
            title="Choose an image",
            filetypes=[("Image files", "*.jpg *.jpeg *.png *.webp *.gif *.tif *.tiff *.bmp"),
                       ("All files", "*.*")],
        )
        if not selected:
            return
        source = Path(selected)
        self.source.set(str(source))
        self.destination.set(str(source.with_name(f"{source.stem}_800x480.bmp")))
        self.update_preview()

    def choose_destination(self) -> None:
        initial = self.destination.get() or "gallery_image.bmp"
        selected = filedialog.asksaveasfilename(
            title="Save converted BMP",
            initialfile=Path(initial).name,
            defaultextension=".bmp",
            filetypes=[("24-bit BMP", "*.bmp")],
        )
        if selected:
            self.destination.set(str(Path(selected).with_suffix(".bmp")))

    def update_preview(self) -> None:
        source = Path(self.source.get())
        if not source.is_file():
            return
        try:
            image = prepare_image(source, self.resize_mode.get(),
                                  ROTATION_OPTIONS[self.rotation.get()])
            image.thumbnail(PREVIEW_SIZE, Image.Resampling.LANCZOS)
            self.preview_image = ImageTk.PhotoImage(image)
            self.preview_label.configure(image=self.preview_image, text="")
            self.status.set("Ready: output will be an uncompressed 24-bit 800 × 480 BMP.")
        except Exception as error:  # Pillow reports malformed or unsupported input here.
            self.preview_label.configure(image="", text="Preview unavailable")
            self.preview_image = None
            self.status.set(f"Could not open image: {error}")

    def convert(self) -> None:
        source = Path(self.source.get())
        destination = Path(self.destination.get())
        try:
            if not self.destination.get().strip():
                raise ValueError("Choose a destination BMP path.")
            convert_image(source, destination, self.resize_mode.get(),
                          ROTATION_OPTIONS[self.rotation.get()])
            if destination.suffix.lower() != ".bmp":
                destination = destination.with_suffix(".bmp")
            self.destination.set(str(destination))
            self.status.set(f"Saved: {destination}")
            messagebox.showinfo("Conversion complete", f"Created 24-bit 800 × 480 BMP:\n{destination}")
        except Exception as error:
            self.status.set(f"Conversion failed: {error}")
            messagebox.showerror("Conversion failed", str(error))


def parse_arguments() -> argparse.Namespace:
    """Parse the optional command-line conversion arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="Source image path")
    parser.add_argument("--output", type=Path, help="Destination BMP path")
    parser.add_argument("--mode", choices=("crop", "fit"), default="crop",
                        help="Resize behavior (default: crop)")
    parser.add_argument("--rotate", choices=(0, 90, 180, 270), type=int, default=0,
                        help="Manual clockwise rotation after EXIF orientation (default: 0)")
    return parser.parse_args()


def main() -> None:
    """Run a one-off conversion or launch the Tk desktop converter."""
    args = parse_arguments()
    if args.input or args.output:
        if not args.input or not args.output:
            raise SystemExit("Both --input and --output are required for command-line conversion.")
        convert_image(args.input, args.output, args.mode, args.rotate)
        print(f"Created {args.output.with_suffix('.bmp')}")
        return

    root = tk.Tk()
    root.title("HMI Gallery Image Converter")
    root.minsize(720, 650)
    style = ttk.Style(root)
    style.configure("Title.TLabel", font=("TkDefaultFont", 18, "bold"))
    ConverterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
