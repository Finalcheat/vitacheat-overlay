"""Strict reader and append-only editor for our pinned Vita RCOF baseline.

Table and tree layouts follow Princess-of-Sleeping/RcoDecompiler. This is not
a general XML compiler: existing payloads remain opaque and byte-identical.
Only linked nodes and their explicit typed attributes are appended. All output
is validated before use. No native PAF execution is implied by validation.
"""
from dataclasses import dataclass
import hashlib
import struct


class RcoError(ValueError):
    pass


@dataclass(frozen=True)
class Attribute:
    name: str
    kind: int
    value: int
    length: int = 0


@dataclass(frozen=True)
class Node:
    offset: int
    name: str
    parent: int
    previous: int
    following: int
    first: int
    last: int
    attributes: tuple

    def attribute(self, name):
        for attribute in self.attributes:
            if attribute.name == name:
                return attribute
        raise RcoError(f"Missing {name} on {self.name}")


def id_hash(identifier):
    return int.from_bytes(hashlib.sha1(identifier.encode("utf-8")).digest()[:4], "big")


class Rco:
    MAGIC = b"RCOF\x10\x01\0\0"

    def __init__(self, data):
        if len(data) < 80 or data[:8] != self.MAGIC:
            raise RcoError("Unsupported RCO header")
        header = struct.unpack_from("<18I", data, 8)
        self.tables = []
        extents = []
        for index in range(0, 18, 2):
            offset, size = header[index:index + 2]
            if offset < 80 or offset % 16 or offset + size > len(data):
                raise RcoError("Invalid table extent/alignment")
            if size:
                extents.append((offset, offset + size))
            self.tables.append(bytearray(data[offset:offset + size]))
        extents.sort()
        if any(left[1] > right[0] for left, right in zip(extents, extents[1:])):
            raise RcoError("Overlapping RCO tables")
        self.validate()

    def string(self, offset):
        table = self.tables[3]
        if not 0 <= offset < len(table):
            raise RcoError("Invalid string offset")
        end = table.find(0, offset)
        if end < 0:
            raise RcoError("Unterminated string")
        try:
            return bytes(table[offset:end]).decode("utf-8")
        except UnicodeDecodeError as error:
            raise RcoError("Invalid resource UTF-8") from error

    def node(self, offset):
        tree = self.tables[0]
        if offset % 4 or not 0 <= offset <= len(tree) - 28:
            raise RcoError("Invalid node offset")
        name, count, parent, previous, following, first, last = struct.unpack_from("<7i", tree, offset)
        if count < 0 or offset + 28 + 16 * count > len(tree):
            raise RcoError("Invalid attribute extent")
        attributes = []
        for index in range(count):
            key, kind, value, length = struct.unpack_from("<4I", tree, offset + 28 + 16 * index)
            attributes.append(Attribute(self.string(key), kind, value, length))
        return Node(offset, self.string(name), parent, previous, following, first, last, tuple(attributes))

    def validate_attribute(self, attribute):
        kind, value, length = attribute.kind, attribute.value, attribute.length
        if kind in (1, 2):
            if length:
                raise RcoError("Scalar with nonzero extent")
            return
        if kind == 3:
            text = self.string(value).encode("utf-8")
            if len(text) != length:
                raise RcoError("String length mismatch")
            return
        if kind in (4, 5, 6, 7, 8):
            table_index = {4: 4, 5: 5, 6: 6, 7: 7, 8: 8}[kind]
            offset = value if kind in (4, 8) else value * 4
            extent = length if kind in (5, 8) else length * (2 if kind == 4 else 4)
            if offset + extent > len(self.tables[table_index]):
                raise RcoError("Attribute payload outside table")
            if kind == 5 and length != 4:
                raise RcoError("Hash must occupy one word")
            return
        if kind in (9, 10):
            table = self.tables[1]
            if value + 4 >= len(table) or table.find(0, value + 4) < 0 or length:
                raise RcoError("Invalid string ID")
            return
        if kind in (11, 12):
            if value % 8 or value + 8 > len(self.tables[2]) or length:
                raise RcoError("Invalid hashed ID")
            return
        raise RcoError("Unsupported attribute kind")

    def validate(self):
        if len(self.tables[2]) % 8:
            raise RcoError("Truncated hashed ID table")
        visited, occupied, result = set(), [], []
        # Iterative traversal bounds recursion and detects cycles/shared nodes.
        pending = [(0, -1, -1)]
        while pending:
            offset, parent, previous = pending.pop()
            if offset in visited:
                raise RcoError("Cyclic/shared resource node")
            visited.add(offset)
            node = self.node(offset)
            if node.parent != parent or node.previous != previous:
                raise RcoError("Inconsistent parent/sibling links")
            if (node.first == -1) != (node.last == -1):
                raise RcoError("Inconsistent child endpoints")
            if len({a.name for a in node.attributes}) != len(node.attributes):
                raise RcoError("Duplicate attribute")
            for attribute in node.attributes:
                self.validate_attribute(attribute)
                if attribute.kind == 11:
                    entity = struct.unpack_from("<i", self.tables[2], attribute.value)[0]
                    if entity != offset:
                        raise RcoError("Registered ID points to a different node")
            occupied.append((offset, offset + 28 + 16 * len(node.attributes)))
            result.append(node)
            if node.following != -1:
                pending.append((node.following, parent, offset))
            elif parent != -1 and self.node(parent).last != offset:
                raise RcoError("Incorrect last-child link")
            if node.first != -1:
                pending.append((node.first, offset, -1))
        occupied.sort()
        if any(left[1] > right[0] for left, right in zip(occupied, occupied[1:])):
            raise RcoError("Overlapping resource nodes")
        for at in range(0, len(self.tables[2]), 8):
            entity = struct.unpack_from("<i", self.tables[2], at)[0]
            if entity != -1 and entity not in visited:
                raise RcoError("Unreachable registered resource")
        return result

    def value(self, attribute):
        if attribute.kind == 1:
            return struct.unpack("<i", struct.pack("<I", attribute.value))[0]
        if attribute.kind == 2:
            return struct.unpack("<f", struct.pack("<I", attribute.value))[0]
        if attribute.kind == 3:
            return self.string(attribute.value)
        if attribute.kind == 5:
            return struct.unpack_from("<I", self.tables[5], attribute.value * 4)[0]
        if attribute.kind in (11, 12):
            return struct.unpack_from("<I", self.tables[2], attribute.value + 4)[0]
        if attribute.kind == 8:
            return bytes(self.tables[8][attribute.value:attribute.value + attribute.length])
        raise RcoError("Unsupported decoded value")

    def find(self, name, identifier=None):
        matches = [n for n in self.validate() if n.name == name and
                   (identifier is None or self.value(n.attribute("id")) == id_hash(identifier))]
        if len(matches) != 1:
            raise RcoError(f"Expected one {name}: {identifier}")
        return matches[0]

    def intern_string(self, text):
        data = text.encode("utf-8") + b"\0"
        table = self.tables[3]
        cursor = 0
        while cursor < len(table):
            end = table.find(0, cursor)
            if end < 0:
                raise RcoError("Unterminated string table")
            if table[cursor:end + 1] == data:
                return cursor
            cursor = end + 1
        offset = len(table)
        table.extend(data)
        return offset

    def hash_attribute(self, name, identifier):
        value = id_hash(identifier)
        table = self.tables[5]
        for at in range(0, len(table), 4):
            if struct.unpack_from("<I", table, at)[0] == value:
                return Attribute(name, 5, at // 4, 4)
        at = len(table)
        table.extend(struct.pack("<I", value))
        return Attribute(name, 5, at // 4, 4)

    def reference(self, name, identifier):
        value, table = id_hash(identifier), self.tables[2]
        matches = [at for at in range(0, len(table), 8)
                   if struct.unpack_from("<I", table, at + 4)[0] == value]
        if len(matches) != 1:
            raise RcoError(f"Missing/ambiguous ID reference: {identifier}")
        return Attribute(name, 12, matches[0])

    def append(self, name, attributes, parent, registered_id=None):
        tree, offset = self.tables[0], len(self.tables[0])
        owner = self.node(parent)
        attributes = list(attributes)
        if registered_id is not None:
            value = id_hash(registered_id)
            if any(struct.unpack_from("<I", self.tables[2], at + 4)[0] == value
                   for at in range(0, len(self.tables[2]), 8)):
                raise RcoError("Registered resource ID collision")
            index = len(self.tables[2])
            self.tables[2].extend(struct.pack("<iI", offset, value))
            attributes.append(Attribute("id", 11, index))
        tree.extend(struct.pack("<7i", self.intern_string(name), len(attributes), parent,
                                owner.last, -1, -1, -1))
        for attribute in attributes:
            tree.extend(struct.pack("<4I", self.intern_string(attribute.name), attribute.kind,
                                    attribute.value, attribute.length))
        if owner.last != -1:
            struct.pack_into("<i", tree, owner.last + 16, offset)
        else:
            struct.pack_into("<i", tree, parent + 20, offset)
        struct.pack_into("<i", tree, parent + 24, offset)
        return offset

    def encode(self):
        self.validate()
        output = bytearray(self.MAGIC + bytes(72))
        for index, table in enumerate(self.tables):
            output.extend(bytes((-len(output)) % 16))
            struct.pack_into("<II", output, 8 + 8 * index, len(output), len(table))
            output.extend(table)
        output.extend(bytes((-len(output)) % 16))
        encoded = bytes(output)
        Rco(encoded)  # Validate rebuilt extents, not only the mutable tables.
        return encoded


def floating(name, value):
    return Attribute(name, 2, struct.unpack("<I", struct.pack("<f", value))[0])
