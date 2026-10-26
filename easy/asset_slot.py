"""Shared layout for the blank .nds asset slot.

The blank ROM reserves a fixed region inside the ARM9 image. make_rom.py
finds PLACEHOLDER_MAGIC and overwrites the whole slot with a real
zelda3_assets.dat (zero-padded). Keep these values in sync between the
blank build and the easy packager.
"""

# Unique marker; must not collide with the real "Zelda3_v0" assets header.
PLACEHOLDER_MAGIC = b"ALTTPDS_ASSET_PLACEHOLDER_v1\0"

# Headroom over the usual ~684 KiB USA asset file. Trailing zeros are fine:
# ZeldaLoadAssets walks the internal size table and ignores the padding.
ASSET_SLOT_SIZE = 768 * 1024


def make_placeholder():
    """Return a SLOT_SIZE blob that starts with PLACEHOLDER_MAGIC."""
    if len(PLACEHOLDER_MAGIC) >= ASSET_SLOT_SIZE:
        raise ValueError("PLACEHOLDER_MAGIC is larger than ASSET_SLOT_SIZE")
    data = bytearray(ASSET_SLOT_SIZE)
    data[: len(PLACEHOLDER_MAGIC)] = PLACEHOLDER_MAGIC
    return bytes(data)


def pack_assets_into_slot(assets):
    """Pad extracted assets to ASSET_SLOT_SIZE for in-place NDS patching."""
    if len(assets) > ASSET_SLOT_SIZE:
        raise ValueError(
            "zelda3_assets.dat is %d bytes; the blank slot only holds %d. "
            "Rebuild the blank with a larger ASSET_SLOT_SIZE."
            % (len(assets), ASSET_SLOT_SIZE)
        )
    return assets + b"\0" * (ASSET_SLOT_SIZE - len(assets))
