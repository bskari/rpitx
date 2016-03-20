"""Hides imports and other irrelevant things so that ipython works nicely."""

from pydub import AudioSegment as _AudioSegment
import StringIO as _StringIO
import _rpitx
import array as _array
import logging as _logging
import wave as _wave


rc_broadcasting = None
rc_parameters_set = None


def _initialize():
    """Module initialization."""
    _rpitx.initialize_rc()
    rc_broadcasting = False
    global rc_broadcasting
    rc_broadcasting = False
    global rc_parameters_set
    rc_parameters_set = False


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


def broadcast_rc():
    """Starts broadcasting RC command signals."""
    if not rc_parameters_set:
        raise ValueError('Broadcast parameters not set')
    if rc_broadcasting:
        return
    global rc_broadcasting
    rc_broadcasting = True
    _rpitx.broadcast_rc()


def stop_broadcasting_rc():
    """Halts the RC broadcast."""
    global rc_broadcasting
    rc_broadcasting = False
    _rpitx.stop_broadcasting_rc()


def set_rc_parameters(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple,
        burst_count
):
    """Changes the currently broadcasting RC command signals."""
    global rc_parameters_set
    rc_parameters_set = True
    _rpitx.set_rc_pwm(
        frequency,
        dead_frequency,
        burst_us,
        synchronization_burst_count,
        synchronization_multiple,
        burst_count
    )
