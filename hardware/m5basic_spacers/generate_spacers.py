#!/usr/bin/env python3
"""Generate chamfered glue-on support spacers for the M5Basic bridge PCB."""

from math import sqrt
from pathlib import Path


OUT = Path(__file__).resolve().parent
WIDTH = 24.0
DEPTH = 8.0
CHAMFER = 1.0
HEIGHTS = [4.0, 4.5, 5.0, 5.5, 6.0, 6.5, 7.0, 7.5, 8.0]


def normal(a, b, c):
    ux, uy, uz = (b[i] - a[i] for i in range(3))
    vx, vy, vz = (c[i] - a[i] for i in range(3))
    nx = uy * vz - uz * vy
    ny = uz * vx - ux * vz
    nz = ux * vy - uy * vx
    length = sqrt(nx * nx + ny * ny + nz * nz)
    return (nx / length, ny / length, nz / length)


def facet(a, b, c):
    nx, ny, nz = normal(a, b, c)
    lines = [f"  facet normal {nx:.8g} {ny:.8g} {nz:.8g}", "    outer loop"]
    for x, y, z in (a, b, c):
        lines.append(f"      vertex {x:.8g} {y:.8g} {z:.8g}")
    lines.extend(["    endloop", "  endfacet"])
    return "\n".join(lines)


def make_mesh(height):
    outline = [
        (CHAMFER, 0.0),
        (WIDTH - CHAMFER, 0.0),
        (WIDTH, CHAMFER),
        (WIDTH, DEPTH - CHAMFER),
        (WIDTH - CHAMFER, DEPTH),
        (CHAMFER, DEPTH),
        (0.0, DEPTH - CHAMFER),
        (0.0, CHAMFER),
    ]
    bottom = [(x, y, 0.0) for x, y in outline]
    top = [(x, y, height) for x, y in outline]
    triangles = []

    for index in range(1, len(outline) - 1):
        triangles.append((bottom[0], bottom[index + 1], bottom[index]))
        triangles.append((top[0], top[index], top[index + 1]))

    for index in range(len(outline)):
        next_index = (index + 1) % len(outline)
        triangles.append((bottom[index], bottom[next_index], top[next_index]))
        triangles.append((bottom[index], top[next_index], top[index]))

    return triangles


def main():
    for height in HEIGHTS:
        label = f"{height:.1f}".replace(".", "p")
        target = OUT / f"m5basic_spacer_24x8_h{label}mm.stl"
        triangles = make_mesh(height)
        body = "\n".join(facet(*triangle) for triangle in triangles)
        target.write_text(
            f"solid m5basic_spacer_h{label}\n{body}\nend solid m5basic_spacer_h{label}\n",
            encoding="ascii",
        )
        print(target.name)


if __name__ == "__main__":
    main()
