"""Media files, as Media layers read them: images, numbered image sequences, video and sound.

Add one as a layer with oanim.ops.media.add(filepath=...); these look at files themselves."""

import _oanim


def probe(filepath, sequence=False):
    """What a file is: a dict with kind ("IMAGE", "SEQUENCE", "VIDEO", "AUDIO" or "NONE"), width and
    height (as decoded, at most 2048 on the longer side), source_width and source_height, duration
    (seconds), fps, frame_count, has_audio and error (why it can't be read)."""
    return dict(_oanim.media_probe(filepath, sequence))


def pixel(filepath, seconds=0.0, x=0, y=0, sequence=False):
    """Decodes the picture `seconds` into a file (waiting for it) and returns (width, height,
    (r, g, b, a) at x, y from the top left, the frame's time), or None. A sequence's image n is at
    n / 1000 seconds."""
    return _oanim.media_pixel(filepath, sequence, float(seconds), int(x), int(y))


def mix(seconds, frames):
    """The open document's sound at `seconds` (frame f is at f / fps) for `frames` samples at 48 kHz:
    a list of stereo samples, left and right interleaved, as playback hears it."""
    return list(_oanim.media_mix(float(seconds), int(frames)))


def missing_count():
    """How many of the document's media files are missing (and not packed)."""
    return _oanim.media_missing_count()
