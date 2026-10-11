import re
from dataclasses import dataclass, field

from board.parts import part

PREFIX = {"SW": "SW", "LED": "D", "JST5": "J", "DEBUG": "TP", "MH": "H"}


def _prefix(key):
    if key in PREFIX:
        return PREFIX[key]
    if key[0] in "RC" and key[1].isdigit():      # R10K, C100N, …
        return key[0]
    return "U"


@dataclass
class PartInstance:
    ref: str
    part: object
    dnp: bool
    value: str


@dataclass
class Circuit:
    instances: dict = field(default_factory=dict)
    nets: dict = field(default_factory=dict)
    _pad_net: dict = field(default_factory=dict)
    _conflicts: list = field(default_factory=list)
    _nc: set = field(default_factory=set)

    def add(self, key, ref=None, dnp=False, value=None):
        p = part(key)
        if ref is None:
            ref = self._next_ref(_prefix(key))
        if ref in self.instances:
            raise ValueError(f"duplicate ref {ref}")
        inst = PartInstance(ref, p, dnp, value or p.value)
        self.instances[ref] = inst
        return inst

    def _pads(self, term):
        inst, name = term[0], term[1]
        if name not in inst.part.pins:
            raise KeyError(f"{inst.ref}: no pin {name!r}")
        if len(term) > 2:
            pad = term[2]
            if pad not in inst.part.pins[name]:
                raise KeyError(f"{inst.ref}: pin {name!r} has no pad {pad!r}")
            return [pad]
        return inst.part.pins[name]

    def _next_ref(self, prefix):
        nums = [int(m.group(1)) for r in self.instances
                if (m := re.fullmatch(re.escape(prefix) + r"(\d+)", r))]
        return f"{prefix}{max(nums, default=0) + 1}"

    def connect(self, net, *terms):
        for t in terms:
            for pad in self._pads(t):
                key = (t[0].ref, pad)
                old = self._pad_net.get(key)
                if old and old != net:
                    self._conflicts.append(f"{key[0]}.{key[1]} on two nets: {old}, {net}")
                    continue
                self._pad_net[key] = net
                self.nets.setdefault(net, set()).add(key)

    def no_connect(self, inst, name):
        for pad in self._pads((inst, name)):
            self._nc.add((inst.ref, pad))

    def net_of(self, ref, pad):
        return self._pad_net.get((ref, pad))


def check(c):
    problems = list(c._conflicts)
    for net, pads in c.nets.items():
        if len(pads) < 2:
            problems.append(f"net {net} has only one pad")
        outs = [k for k in pads if c.instances[k[0]].part.pin_types.get(k[1]) == "output"]
        if len(outs) > 1:
            problems.append(f"net {net} has several outputs: {outs}")
    for ref, inst in c.instances.items():
        if inst.dnp:
            continue
        for pad, typ in inst.part.pin_types.items():
            if typ in ("power_in", "power_out") and (ref, pad) not in c._pad_net and (ref, pad) not in c._nc:
                problems.append(f"{ref} pad {pad}: power pin unconnected")
    return problems
