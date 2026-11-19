#!/usr/bin/env python3
"""Create red SD-card test artwork from the built-in eye source images."""

from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets"
SD_ROOT = ROOT / "sd_card_example"
SD_ASSETS = SD_ROOT / "assets"
SIZE = (480, 800)
RED = (205, 20, 20)
WHITE = (255, 255, 255)


def make_red(source: Path, destination: Path) -> None:
    image = Image.open(source).convert("RGB")
    image = ImageOps.fit(image, SIZE, method=Image.Resampling.LANCZOS)
    mask = ImageOps.grayscale(image).point(lambda value: 255 if value < 210 else 0)
    output = Image.new("RGB", SIZE, WHITE)
    output.paste(Image.new("RGB", SIZE, RED), mask=mask)
    output.save(destination, format="JPEG", quality=96, subsampling=0)


def main() -> None:
    SD_ASSETS.mkdir(parents=True, exist_ok=True)
    make_red(ASSETS / "me_close.jpg", SD_ASSETS / "me_close.jpg")
    make_red(ASSETS / "me_open.jpg", SD_ASSETS / "me_open.jpg")
    (SD_ROOT / "setting.txt").write_text(
        "# e-paper schedule settings\n"
        "INITIAL_TIME=12:00\n"
        "START_TIME=18:00\n"
        "END_TIME=09:00\n",
        encoding="utf-8",
    )
    print(SD_ROOT)


if __name__ == "__main__":
    main()
