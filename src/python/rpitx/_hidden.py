"""Hides imports and other irrelevant things so that ipython works nicely."""

from pydub import AudioSegment as _AudioSegment
import StringIO as _StringIO
import _rpitx
import array as _array
import logging as _logging
import wave as _wave


rc_broadcasting = None

def _initialize():
    """Module initialization."""
    print('Initialize')
    _rpitx.initialize_rc()
    print('Done initialize')
    global rc_broadcasting
    rc_broadcasting = False


_initialize()


def broadcast_fm(file_, frequency):
    """Play a music file over FM."""

    _logging.basicConfig()
    logger = _logging.getLogger('rpitx')

    def _reencode(file_name):
        """Returns an AudioSegment file reencoded to the proper WAV format."""
        original = _AudioSegment.from_file(file_name)
        if original.channels > 2:
            raise ValueError('Too many channels in sound file')
        if original.channels == 2:
            # TODO: Support stereo. For now, just overlay into mono.
            logger.info('Reducing stereo channels to mono')
            left, right = original.split_to_mono()
            original = left.overlay(right)

        return original

    raw_audio_data = _reencode(file_)

    wav_data = _StringIO.StringIO()
    wav_writer = _wave.open(wav_data, 'w')
    wav_writer.setnchannels(1)
    wav_writer.setsampwidth(2)
    wav_writer.setframerate(48000)
    wav_writer.writeframes(raw_audio_data.raw_data)
    wav_writer.close()

    raw_array = _array.array('c', wav_data.getvalue())
    array_address, length = raw_array.buffer_info()
    _rpitx.broadcast_fm(array_address, length, frequency)


def broadcast_rc(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple
):
    """Starts broadcasting RC command signals.

    Args:
    frequency (float) -- Frequency to broadcast on.
    dead_frequency (float) -- Frequency to broadcast on for gaps.
    burst_us (int) -- Base time in nanosecnds.
    synchronization_burst_count (int) -- Number of bursts for synch signal.
    synch_multiple (int) -- Length of synch burst as multiple of burstUs.
    burst_count (int) -- Number of bursts for command signal.
    """
    set_rc_parameters(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple
    )
    global rc_broadcasting
    rc_broadcasting = True
    _rpitx.broadcast_rc()


def stop_broadcasting_rc():
    """Halts the RC broadcast."""
    _rpitx.stop_broadcasting_rc()


def set_rc_parameters(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple
):
    """Changes the currently broadcasting RC command signals."""
    if not rc_broadcasting:
        raise ValueError('RC is not broadcasting')

    _rpitx.set_rc_pwm(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple
    )
