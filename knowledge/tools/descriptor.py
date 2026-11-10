# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Read a manufacturer JSON descriptor without losing where each value is.

A descriptor is evidence as well as data: a record derived from it has to cite
the lines that state the fact. `parse` returns the same values `json.loads`
would, each wrapped with the one-based lines it occupies in the pinned file.
"""
import bisect
import json

_SCALAR = json.JSONDecoder()
_SPACE = " \t\r\n"


class Located:
    """A JSON value and the lines it spans. `items` holds the located members
    of an object (by key) or of an array (by position)."""
    __slots__ = ("value", "start", "end", "items")

    def __init__(self, value, start, end, items=None):
        self.value, self.start, self.end, self.items = value, start, end, items

    def __getitem__(self, key):
        return self.items[key]

    def get(self, key):
        return self.items.get(key) if isinstance(self.items, dict) else None

    def __iter__(self):
        return iter(self.items if isinstance(self.items, list) else self.items.values())


def parse(source) -> Located:
    """Parse a pinned source (anything with `.id` and `.lines`) as JSON."""
    text = "\n".join(source.lines)
    starts = [0]
    for index, character in enumerate(text):
        if character == "\n":
            starts.append(index + 1)

    def line(offset):
        return bisect.bisect_right(starts, offset)

    def skip(offset):
        while offset < len(text) and text[offset] in _SPACE:
            offset += 1
        return offset

    def value(offset):
        offset = skip(offset)
        opener = text[offset]
        if opener == "{":
            members, plain, cursor = {}, {}, skip(offset + 1)
            while text[cursor] != "}":
                key, cursor = _SCALAR.raw_decode(text, cursor)
                cursor = skip(cursor)
                if text[cursor] != ":" or key in members:
                    raise SystemExit(f"{source.id}: unreadable or repeated member near line {line(cursor)}")
                members[key], cursor = value(cursor + 1)
                plain[key] = members[key].value
                cursor = skip(cursor)
                if text[cursor] == ",":
                    cursor = skip(cursor + 1)
            return Located(plain, line(offset), line(cursor), members), cursor + 1
        if opener == "[":
            elements, cursor = [], skip(offset + 1)
            while text[cursor] != "]":
                element, cursor = value(cursor)
                elements.append(element)
                cursor = skip(cursor)
                if text[cursor] == ",":
                    cursor = skip(cursor + 1)
            return Located([element.value for element in elements], line(offset), line(cursor), elements), cursor + 1
        scalar, end = _SCALAR.raw_decode(text, offset)
        return Located(scalar, line(offset), line(end - 1)), end

    try:
        root, end = value(0)
    except (IndexError, ValueError) as error:
        raise SystemExit(f"{source.id}: not readable as JSON: {error}")
    if skip(end) != len(text):
        raise SystemExit(f"{source.id}: text after the JSON value")
    return root
