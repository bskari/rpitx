#include <Python.h>
#include <assert.h>
#include <endian.h>
#include <sndfile.h>

#include "../RpiTx.h"
#include "../RpiGpio.h"

#define COUNT_OF(x) ((sizeof(x)/sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))

struct module_state {
	PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
static struct module_state _state;
#endif

typedef struct {
	double frequency;
	uint32_t waitForThisSample;
	int32_t padding;
} samplerf_t;

// WAV variables and prototypes
static void* sampleBase = NULL;
static sf_count_t sampleLength = 0;
static sf_count_t sampleOffset = 0;
static SNDFILE* sndFile = NULL;
static int bitRate;

static sf_count_t virtualSndfileGetLength(void* unused) __attribute__((warn_unused_result));
static sf_count_t virtualSndfileRead(void* const dest, const sf_count_t count, void* const userData) __attribute__((warn_unused_result));
static sf_count_t virtualSndfileTell(void* const unused) __attribute__((warn_unused_result));
static ssize_t formatRfWrapper(void* const outBuffer, const size_t count) __attribute__((warn_unused_result));
static void resetSndFile(void);

// SSTV variables and prototypes
typedef enum ColorChannelType {
	SEPARATOR,
	RED,
	GREEN,
	BLUE
} ColorChannelType;
static unsigned char* bmpData = NULL;
static ssize_t formatSstv(void* outBuffer, const size_t count) __attribute__((warn_unused_result));;
static ssize_t addSstvHeader(samplerf_t** outBuffer) __attribute__((warn_unused_result));
static ssize_t addSstvTrailer(samplerf_t** outBuffer) __attribute__((warn_unused_result));
static ssize_t sstvTone(double frequency, uint32_t timing, samplerf_t** outBuffer) __attribute__((warn_unused_result));


// These methods used by libsndfile's virtual file open function
static sf_count_t virtualSndfileGetLength(void* unused) {
	return sampleLength;
}
static sf_count_t virtualSndfileRead(void* const dest, const sf_count_t count, void* const userData) {
	const sf_count_t bytesAvailable = sampleLength - sampleOffset;
	const int numBytes = bytesAvailable > count ? count : bytesAvailable;
	memcpy(dest, ((char*)userData) + sampleOffset, numBytes);
	sampleOffset += numBytes;
	return numBytes;
}
static sf_count_t virtualSndfileTell(void* const unused) {
	return sampleOffset;
}
static sf_count_t virtualSndfileSeek(
		const sf_count_t offset,
		const int whence,
		void* const unused
) {
	switch (whence) {
		case SEEK_CUR:
			sampleOffset += offset;
			break;
		case SEEK_SET:
			sampleOffset = offset;
			break;
		case SEEK_END:
			sampleOffset = sampleLength - offset;
			break;
		default:
			assert(!"Invalid whence");
	}
	return sampleOffset;
}


/**
 * Formats a chunk of an array of a mono 44k wav at a time and outputs IQ
 * formatted array for broadcast.
 */
static ssize_t formatRfWrapper(void* const outBuffer, const size_t count) {
	static float wavBuffer[1024];
	static int wavOffset = -1;
	static int wavFilled = -1;

	if (wavFilled == 0) {
		return 0;
	}

	const int excursion = 6000;
	int numBytesWritten = 0;
	samplerf_t samplerf;
	samplerf.padding = 0;
	samplerf.waitForThisSample = 1e9 / ((float)bitRate);  //en 100 de nanosecond
	char* const out = outBuffer;

	while (numBytesWritten < count) {
		for (
				;
				numBytesWritten <= count - sizeof(samplerf_t) && wavOffset < wavFilled;
				++wavOffset
		) {
			const float x = wavBuffer[wavOffset];
			samplerf.frequency = x * excursion * 2.0;
			memcpy(&out[numBytesWritten], &samplerf, sizeof(samplerf_t));
			numBytesWritten += sizeof(samplerf_t);
		}

		assert(wavOffset <= wavFilled);

		if (wavOffset == wavFilled) {
			wavFilled = sf_readf_float(sndFile, wavBuffer, COUNT_OF(wavBuffer));
			wavOffset = 0;
		}
	}
	return numBytesWritten;
}
static void resetSndFile(void) {
	sampleOffset = 0;
}


static ssize_t formatSstv(void* outBuffer, const size_t count) {
	const int ROWS = 320;
	const int COLUMNS = 240;
	const int BYTES_PER_CHANNEL = ROWS * sizeof(samplerf_t) + sizeof(samplerf_t);
	const double frequencyMartin1[3] = {1200, 1500, 1500};
	const double timingMartin1[3] = {48720, 5720, 4576};

	int headerSet = 0;
	static int row = 0;
	static int column = 0;
	static ColorChannelType colorChannel = SEPARATOR;

	int bytesWrittenCount = 0;
	samplerf_t* buffer = outBuffer;

	if (!headerSet) {
		bytesWrittenCount += addSstvHeader(&buffer);
		bytesWrittenCount += addSstvTrailer(&buffer);
		headerSet = 1;
		assert(bytesWrittenCount < count && "Header is too big");
	}

	while (column < COLUMNS) {
		int before;
		switch (colorChannel) {
			case SEPARATOR:
				if (bytesWrittenCount + 2 * sizeof(samplerf_t) > count) {
					return bytesWrittenCount;
				}
				before = bytesWrittenCount;
				// Horizontal SYNC
				bytesWrittenCount += sstvTone(frequencyMartin1[0], timingMartin1[0], &buffer);
				// Separator Tone
				bytesWrittenCount += sstvTone(frequencyMartin1[1], timingMartin1[1], &buffer);

				colorChannel = GREEN;
				break;

			case GREEN:
				if (bytesWrittenCount + BYTES_PER_CHANNEL > count) {
					return bytesWrittenCount;
				}
				before = bytesWrittenCount;
				for (row = 0; row < ROWS; ++row) {
					bytesWrittenCount += sstvTone(
						frequencyMartin1[1] + bmpData[row * 3 + 0] * 800 / 256,
						timingMartin1[2],
						&buffer
					);
				}
				bytesWrittenCount += sstvTone(
					frequencyMartin1[1],
					timingMartin1[1],
					&buffer
				);
				colorChannel = BLUE;
				break;

			case BLUE:
				if (bytesWrittenCount + BYTES_PER_CHANNEL > count) {
					return bytesWrittenCount;
				}
				before = bytesWrittenCount;
				for (row = 0; row < ROWS; ++row) {
					bytesWrittenCount += sstvTone(
						frequencyMartin1[1] + bmpData[row * 3 + 1] * 800 / 256,
						timingMartin1[2],
						&buffer
					);
				}
				bytesWrittenCount += sstvTone(
					frequencyMartin1[1],
					timingMartin1[1],
					&buffer
				);
				colorChannel = RED;
				break;

			case RED:
				if (bytesWrittenCount + BYTES_PER_CHANNEL > count) {
					return bytesWrittenCount;
				}
				before = bytesWrittenCount;
				for (row = 0; row < ROWS; ++row) {
					bytesWrittenCount += sstvTone(
						frequencyMartin1[1] + bmpData[row * 3 + 2] * 800 / 256,
						timingMartin1[2],
						&buffer
					);
				}
				// Go to the next line
				bmpData += ROWS * 3;
				bytesWrittenCount += sstvTone(
					frequencyMartin1[1],
					timingMartin1[1],
					&buffer
				);
				colorChannel = SEPARATOR;
				++column;
				break;

			default:
				assert(0 && "Invalid ColorChannelType");
		}
	}

	return bytesWrittenCount;
}


static ssize_t addSstvHeader(samplerf_t** outBuffer) {
	ssize_t bytesWrittenCount = 0;
	// bit of silence
	bytesWrittenCount += sstvTone(0, 5000000, outBuffer);

	// attention tones
	bytesWrittenCount += sstvTone(1900, 100000, outBuffer);
	bytesWrittenCount += sstvTone(1500, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(1900, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(1500, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(2300, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(1500, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(2300, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(1500, 1000000, outBuffer);

	// VIS lead, break, mid, start
	bytesWrittenCount += sstvTone(1900, 3000000, outBuffer);
	bytesWrittenCount += sstvTone(1200, 100000, outBuffer);
	bytesWrittenCount += sstvTone(1900, 3000000, outBuffer);
	bytesWrittenCount += sstvTone(1200, 300000, outBuffer);

	// VIS data bits (Martin 1)
	bytesWrittenCount += sstvTone(1300, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1300, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1100, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1100, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1300, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1100, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1300, 300000, outBuffer);
	bytesWrittenCount += sstvTone(1100, 300000, outBuffer);

	// VIS stop
	bytesWrittenCount += sstvTone(1200, 300000, outBuffer);

	return bytesWrittenCount;
}


static ssize_t addSstvTrailer(samplerf_t** outBuffer) {
	ssize_t bytesWrittenCount = 0;
	bytesWrittenCount += sstvTone(2300, 3000000, outBuffer);
	bytesWrittenCount += sstvTone(1200, 100000, outBuffer);
	bytesWrittenCount += sstvTone(2300, 1000000, outBuffer);
	bytesWrittenCount += sstvTone(1200, 300000, outBuffer);

	// bit of silence
	bytesWrittenCount += sstvTone(0, 5000000, outBuffer);
	return bytesWrittenCount;
}


static ssize_t sstvTone(double frequency, uint32_t timing, samplerf_t** outBuffer) {
	(*outBuffer)->frequency = frequency;
	(*outBuffer)->waitForThisSample = timing * 100;
	*outBuffer = *outBuffer + 1;
	return sizeof(samplerf_t);
}


static PyObject*
_rpitx_broadcast_fm(PyObject* self, PyObject* args) {
	float frequency;

	assert(sizeof(sampleBase) == sizeof(unsigned long));
	if (!PyArg_ParseTuple(args, "Lif", &sampleBase, &sampleLength, &frequency)) {
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, "Invalid arguments");
		return NULL;
	}

	sampleOffset = 0;

	SF_VIRTUAL_IO virtualIo = {
		.get_filelen = virtualSndfileGetLength,
		.seek = virtualSndfileSeek,
		.read = virtualSndfileRead,
		.write = NULL,
		.tell = virtualSndfileTell
	};
	SF_INFO sfInfo;
	sndFile = sf_open_virtual(&virtualIo, SFM_READ, &sfInfo, sampleBase);
	if (sf_error(sndFile) != SF_ERR_NO_ERROR) {
		char message[100];
		snprintf(
				message,
				COUNT_OF(message),
				"Unable to open sound file: %s",
				sf_strerror(sndFile));
		message[COUNT_OF(message) - 1] = '\0';
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, message);
		return NULL;
	}
	bitRate = sfInfo.samplerate;

	int skipSignals[] = {
		SIGALRM,
		SIGVTALRM,
		SIGCHLD,  // We fork whenever calling broadcast_fm
		SIGWINCH,  // Window resized
		0
	};
	pitx_run(MODE_RF, bitRate, frequency * 1000.0, 0.0, 0, formatRfWrapper, resetSndFile, skipSignals, 0);
	sf_close(sndFile);

	Py_RETURN_NONE;
}


static PyObject*
_rpitx_broadcast_sstv(PyObject* self, PyObject* args) {
	float frequency;

	assert(sizeof(bmpData) == sizeof(unsigned long));
	if (!PyArg_ParseTuple(args, "Lf", &bmpData, &frequency)) {
		struct module_state* st = GETSTATE(self);
		PyErr_SetString(st->error, "Invalid arguments");
		return NULL;
	}

	// Jump bmpData to where the actual RGB values start
	const uint32_t rgbByteOffset = le32toh(*(uint32_t*)(bmpData + 10));
	bmpData += rgbByteOffset;

	int skipSignals[] = {
		SIGALRM,
		SIGVTALRM,
		SIGCHLD,  // We fork whenever calling broadcast_fm
		SIGWINCH,  // Window resized
		0
	};
	pitx_run(
		MODE_RF,
		0,  // Bit rate, ignored for MODE_RF
		frequency * 1000.0,
		0.0,  // ppmpll
		0,  // NoUsePwmFrequency
		formatSstv,
		NULL,  // reset, not allowed for SSTV
		skipSignals,
		0  // SetDma
	);

	Py_RETURN_NONE;
}


static PyMethodDef _rpitx_methods[] = {
	{
		"broadcast_fm",
		_rpitx_broadcast_fm,
		METH_VARARGS,
		"Low-level broadcasting.\n\n"
			"Broadcast a WAV formatted 48KHz memory array.\n"
			"Args:\n"
			"    address (int): Address of the memory array.\n"
			"    length (int): Length of the memory array.\n"
			"    frequency (float): The frequency, in MHz, to broadcast on.\n"
	},
	{
		"broadcast_sstv",
		_rpitx_broadcast_sstv,
		METH_VARARGS,
		"Low-level broadcasting.\n\n"
			"Broadcast an 320x240 BMP file over SSTV.\n"
			"Args:\n"
			"    address (int): Address of the BMP memory array.\n"
			"    frequency (float): The frequency, in MHz, to broadcast on.\n"
	},
	{NULL, NULL, 0, NULL}
};


#if PY_MAJOR_VERSION >= 3
static int _rpitx_traverse(PyObject* m, visitproc visit, void* arg) {
	Py_VISIT(GETSTATE(m)->error);
	return 0;
}


static int _rpitx_clear(PyObject *m) {
	Py_CLEAR(GETSTATE(m)->error);
	return 0;
}


static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"_rpitx",
	NULL,
	sizeof(struct module_state),
	_rpitx_methods,
	NULL,
	_rpitx_traverse,
	_rpitx_clear,
	NULL
};

#define INITERROR return NULL

PyObject*
PyInit__rpitx(void)

#else

#define INITERROR return

void
init_rpitx(void)
#endif
{
#if PY_MAJOR_VERSION >= 3
	PyObject* const module = PyModule_Create(&moduledef);
#else
	PyObject* const module = Py_InitModule("_rpitx", _rpitx_methods);
#endif
	if (module == NULL) {
		INITERROR;
	}
	struct module_state* st = GETSTATE(module);
	st->error = PyErr_NewException("_rpitx.Error", NULL, NULL);
	if (st->error == NULL) {
		Py_DECREF(module);
		INITERROR;
	}
	Py_INCREF(st->error);
	PyModule_AddObject(module, "error", st->error);

#if PY_MAJOR_VERSION >= 3
	return module;
#endif
}
