"""The capture profile: capture-profile.json, and the run shape that replay, CI and the ledger share."""
from dataclasses import dataclass
import json
from pathlib import Path

PROFILE = Path(__file__).resolve().parent / 'capture-profile.json'


@dataclass(frozen=True)
class RunShape:
    """What one complete capture covers at each size."""
    schemes: dict  # Theme name, as used in shot names, to its KDE colour scheme, in capture order.
    sizes: tuple  # Logical (width, height) outputs. The first is the default and carries documentation.
    repeat: int  # Clean replays per theme at each size.
    kdeglobals_fonts: dict  # [General] font entries for the private kdeglobals.

    @property
    def heights(self):
        return tuple(height for _, height in self.sizes)

    def size(self, height):
        """The (width, height) output for a profile height. Raises ValueError for any other height."""
        for size in self.sizes:
            if size[1] == height:
                return size
        raise ValueError(f'{height} is not a logical height in {PROFILE.name}')


def load(path=PROFILE):
    return json.loads(Path(path).read_text())


def run_shape(profile):
    """Read the run shape from a loaded profile. Raises ValueError when it is malformed."""
    environment = profile['environment']
    shape = RunShape(schemes=dict(environment['colour_schemes']),
                     sizes=tuple(tuple(size) for size in environment['logical_sizes']),
                     repeat=profile['clean_replays'],
                     kdeglobals_fonts=dict(profile['requested_fonts']['kdeglobals']))
    if not shape.schemes or not all(isinstance(v, str) and v for v in shape.schemes.values()):
        raise ValueError('environment.colour_schemes must map theme names to colour scheme names')
    if not shape.sizes or not all(len(size) == 2 and all(type(n) is int and n > 0 for n in size)
                                  for size in shape.sizes):
        raise ValueError('environment.logical_sizes must list positive [width, height] pairs')
    if len(set(shape.heights)) != len(shape.heights):
        raise ValueError('environment.logical_sizes must not repeat a height')
    if type(shape.repeat) is not int or shape.repeat < 2:
        raise ValueError('clean_replays must be an integer of at least 2 for pixel comparison')
    return shape
