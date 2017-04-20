"""Hides imports and other irrelevant things so that ipython works nicely."""

from __future__ import absolute_import, division, print_function
from builtins import (
    bytes, str, open, super, range, zip, round, input, int, pow, object
)

from PIL import Image
from pydub import AudioSegment
import _rpitx
import array
import io
import logging
import sys
import wave


if sys.version_info.major >= 3:
    BufferIO = io.StringIO
else:
    BufferIO = io.BytesIO


def broadcast_fm(file_name, frequency):
    """Play a music file over FM."""

    logging.basicConfig()
    logger = logging.getLogger('rpitx')

    def _reencode(file_name):
        """Returns an AudioSegment file reencoded to the proper WAV format."""
        original = AudioSegment.from_file(file_name)
        if original.channels > 2:
            raise ValueError('Too many channels in sound file')
        if original.channels == 2:
            # TODO: Support stereo. For now, just overlay into mono.
            logger.info('Reducing stereo channels to mono')
            left, right = original.split_to_mono()
            original = left.overlay(right)

        return original

    raw_audio_data = _reencode(file_name)

    wav_data = BufferIO()
    wav_writer = wave.open(wav_data, 'w')
    wav_writer.setnchannels(1)
    wav_writer.setsampwidth(2)
    wav_writer.setframerate(48000)
    wav_writer.writeframes(raw_audio_data.raw_data)
    wav_writer.close()

    raw_array = array.array('c', wav_data.getvalue())
    array_address, length = raw_array.buffer_info()
    _rpitx.broadcast_fm(array_address, length, frequency)


def broadcast_sstv(file_name, frequency):
    """Broadcast a picture over SSTV."""
    logging.basicConfig()
    logger = logging.getLogger('rpitx')

    original_image = Image.open(file_name)
    original_size = original_image.size
    sstv_size = (320, 256)
    # Preserve the aspect ratio
    scale = min((float(sstv_size[i]) / original_size[i] for i in range(2)))
    new_size = [int(original_size[i] * scale) for i in range(2)]
    scaled_image = original_image.resize(new_size)

    # Paste the image into the center of a black image
    resized_image = Image.new('RGB', sstv_size)
    center = (
        (sstv_size[0] - new_size[0]) // 2,
        (sstv_size[1] - new_size[1]) // 2,
    )
    resized_image.paste(scaled_image, center)

    resized_image = resized_image.convert('RGB')
    bmp_data = BufferIO()
    resized_image.save(bmp_data, 'BMP')
    raw_array = array.array('c', bmp_data.getvalue())
    array_address, length = raw_array.buffer_info()
    _rpitx.broadcast_ft(array_address, length, frequency)
