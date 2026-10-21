#!/usr/bin/env python3
"""Convert one GPX track segment or route to the example's route CSV.

Uses Python's standard library. Disconnected segments are never joined, and
oversized routes are rejected rather than silently changing their geometry.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import tempfile
import xml.etree.ElementTree as ET

MAX_INPUT_BYTES = 8 * 1024 * 1024
MAX_POINTS = 1024
GPX_NAMESPACES = {"", "http://www.topografix.com/GPX/1/0", "http://www.topografix.com/GPX/1/1"}


def positive_index(text: str) -> int:
    try:
        index = int(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("use a positive 1-based index") from error
    if index < 1:
        raise argparse.ArgumentTypeError("use a positive 1-based index")
    return index


def select(items: list[ET.Element], index: int | None, label: str) -> ET.Element:
    if not items:
        raise ValueError(f"GPX has no {label}")
    if index is None:
        if len(items) != 1:
            raise ValueError(f"GPX has {len(items)} {label}s; select one with --{label}")
        return items[0]
    if index < 1 or index > len(items):
        raise ValueError(f"--{label} must be in 1..{len(items)}")
    return items[index - 1]


def convert_gpx(data: bytes, *, track: int | None = None, segment: int | None = None,
                route: int | None = None) -> list[tuple[float, float]]:
    if len(data) > MAX_INPUT_BYTES:
        raise ValueError("GPX input exceeds 8 MiB")
    # Do not accept DTD/entity expansion. Removing NUL also covers UTF-16/32
    # spellings of these ASCII declarations before ElementTree parses them.
    declarations = data.replace(b"\x00", b"").upper()
    if b"<!DOCTYPE" in declarations or b"<!ENTITY" in declarations:
        raise ValueError("GPX DTD/entity declarations are not supported")
    try:
        root = ET.fromstring(data)
    except ET.ParseError as error:
        raise ValueError(f"invalid GPX XML: {error}") from error
    namespace = root.tag[1:].split("}", 1)[0] if root.tag.startswith("{") else ""
    prefix = "{" + namespace + "}" if namespace else ""
    if root.tag != prefix + "gpx" or namespace not in GPX_NAMESPACES:
        raise ValueError("expected a GPX 1.0/1.1 root element")
    if route is not None and (track is not None or segment is not None):
        raise ValueError("--route cannot be combined with --track or --segment")
    tracks = root.findall(prefix + "trk")
    routes = root.findall(prefix + "rte")
    if route is not None:
        chosen = select(routes, route, "route")
        elements = chosen.findall(prefix + "rtept")
    else:
        if track is None and segment is None and tracks and routes:
            raise ValueError("GPX contains tracks and routes; choose --track or --route")
        if tracks or track is not None or segment is not None:
            chosen = select(tracks, track, "track")
            chosen_segment = select(chosen.findall(prefix + "trkseg"), segment, "segment")
            elements = chosen_segment.findall(prefix + "trkpt")
        else:
            chosen = select(routes, None, "route")
            elements = chosen.findall(prefix + "rtept")
    points: list[tuple[float, float]] = []
    for index, element in enumerate(elements, 1):
        try:
            latitude = float(element.attrib["lat"])
            longitude = float(element.attrib["lon"])
        except (KeyError, ValueError) as error:
            raise ValueError(f"point {index} needs numeric lat and lon attributes") from error
        if (not math.isfinite(latitude) or not math.isfinite(longitude)
                or not -90 <= latitude <= 90 or not -180 <= longitude <= 180):
            raise ValueError(f"point {index} has an out-of-range coordinate")
        # Match output precision before deduplication; repeated loop endpoints
        # remain, while consecutive zero-length segments are discarded.
        point = (round(latitude, 7), round(longitude, 7))
        if not points or point != points[-1]:
            points.append(point)
        if len(points) > MAX_POINTS:
            raise ValueError("route exceeds 1024 points; simplify it in your route editor first")
    if len(points) < 2:
        raise ValueError("selected path needs at least two distinct consecutive points")
    return points


def csv_text(points: list[tuple[float, float]]) -> str:
    return "latitude,longitude\n" + "".join(f"{lat:.7f},{lon:.7f}\n" for lat, lon in points)


def write_output(path: Path, content: str, force: bool) -> None:
    if not force:
        # Exclusive creation also prevents accidentally replacing the GPX input.
        with path.open("x", encoding="ascii", newline="\n") as output:
            output.write(content)
        return
    temporary: str | None = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="ascii", newline="\n",
                                         dir=path.parent, prefix=path.name + ".",
                                         suffix=".tmp", delete=False) as output:
            temporary = output.name
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            Path(temporary).unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="GPX 1.0/1.1 file")
    parser.add_argument("output", type=Path, help="CSV to copy to SD /routes/bike.csv")
    choice = parser.add_mutually_exclusive_group()
    choice.add_argument("--track", type=positive_index, help="1-based track index")
    choice.add_argument("--route", type=positive_index, help="1-based route index")
    parser.add_argument("--segment", type=positive_index, help="1-based segment of chosen track")
    parser.add_argument("--force", action="store_true", help="replace an existing output CSV")
    args = parser.parse_args(argv)
    try:
        if args.input.resolve() == args.output.resolve():
            raise ValueError("input and output must be different files")
        with args.input.open("rb") as source:
            data = source.read(MAX_INPUT_BYTES + 1)
        points = convert_gpx(data, track=args.track, segment=args.segment, route=args.route)
        write_output(args.output, csv_text(points), args.force)
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {len(points)} points to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
