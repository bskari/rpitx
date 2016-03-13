#include <Python.h>
#include <assert.h>
#include <pthread.h>
#include <sndfile.h>

#include "../RpiTx.h"
#include "../RpiGpio.h"
#include "../RcPwm.h"

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

static void* sampleBase;
static sf_count_t sampleLength;
static sf_count_t sampleOffset;
static SNDFILE* sndFile;
static int bitRate;

static int rcRunning;
static pthread_t rcThread;
static int rcInitialized;

static int skipSignals[] = {
	SIGALRM,
	SIGVTALRM,
	SIGCHLD,  // We fork whenever calling broadcast_fm
	SIGWINCH,  // Window resized
	0
};


/* These methods used by libsndfile's virtual file open function */
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


typedef struct {
	double frequency;
	uint32_t waitForThisSample;
} samplerf_t;
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
static void reset(void) {
	sampleOffset = 0;
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

	pitx_run(MODE_RF, bitRate, frequency * 1000.0, 0.0, 0, formatRfWrapper, reset, skipSignals);
	sf_close(sndFile);

	Py_RETURN_NONE;
}


static PyObject*
_rpitx_set_rc_pwm(PyObject* self, PyObject* args) {
	float frequency;
	float deadFrequency;
	int burstUs;
	int synchronizationBurstCount;
	int synchronizationMultiple;
	int burstCount;

	if (!PyArg_ParseTuple(
				args,
				"ffiiii",
				&frequency,
				&deadFrequency,
				&burstUs,
				&synchronizationBurstCount,
				&synchronizationMultiple,
				&burstCount
	)) {
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, "Invalid arguments");
		return NULL;
	}

	Py_RETURN_NONE;
}


static PyObject*
_rpitx_rc_initialize(PyObject* self) {
	if (rcInitialize() != 0) {
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, "Failed to initialize");
		return NULL;
	}
	rcInitialized = 1;
	Py_RETURN_NONE;
}



static void rc_pthread_start_routine(void* unused) {
	// TODO(2016-03-05) Is this bit rate correct? Does the frequency matter?
	printf("rc_start_routine calling pitx_run\n");
	fflush(stdout);
	const float RC_27_MIDDLE_CHANNEL = 27.145;

    // Magic value, not used except to calculate time to sleep in RF mode
    const int bitRate = 100000;

	pitx_run(MODE_RF, bitRate, RC_27_MIDDLE_CHANNEL * 1000.0, 0.0, 0, rcFillBuffer, NULL, skipSignals);
	printf("rc_start_routine exiting\n");
	fflush(stdout);
}
static PyObject*
_rpitx_broadcast_rc(PyObject* self) {
	if (!rcInitialized) {
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, "Need to call rc_initialize first");
		return NULL;
	}
	if (rcRunning) {
		struct module_state *st = GETSTATE(self);
		PyErr_SetString(st->error, "Already running");
		return NULL;
	}

	rcRunning = 1;
	const int status = pthread_create(&rcThread, NULL, rc_pthread_start_routine, NULL);
	if (status != 0) {
		struct module_state *st = GETSTATE(self);
		char message[100];
		snprintf(
				message,
				COUNT_OF(message),
				"Unable to create thread, error number: %d",
				status);
		PyErr_SetString(st->error, message);
		return NULL;
	}

	Py_RETURN_NONE;
}


static PyObject*
_rpitx_stop_broadcasting_rc(PyObject* self) {
	printf("_rpitx_stop_broadcasting_rc calling rcStop\n");
	fflush(stdout);
	rcStop();
	printf("_rpitx_stop_broadcasting_rc joining\n");
	fflush(stdout);
	pthread_join(rcThread, NULL);
	printf("_rpitx_stop_broadcasting_rc done\n");
	fflush(stdout);
	Py_RETURN_NONE;
}


static PyMethodDef _rpitx_methods[] = {
	// FM audio
	{
		"broadcast_fm",
		_rpitx_broadcast_fm,
		METH_VARARGS,
		"Low-level broadcasting of FM audio.\n\n"
			"Broadcast a WAV formatted 48KHz memory array.\n"
			"Args:\n"
			"	address (int): Address of the memory array.\n"
			"	length (int): Length of the memory array.\n"
			"	frequency (float): The frequency, in MHz, to broadcast on.\n"
	},
	// RC
	{
		"initialize_rc",
		(PyCFunction)_rpitx_rc_initialize,
		METH_NOARGS,
		"Initializes the Pi for RC broadcasting."
	},
	{
		"broadcast_rc",
		(PyCFunction)_rpitx_broadcast_rc,
		METH_NOARGS,
		"Starts broadcasting RC control signals."
	},
	{
		"stop_broadcasting_rc",
		(PyCFunction)_rpitx_stop_broadcasting_rc,
		METH_NOARGS,
		"Stops broadcasting RC control signals."
	},
	{
		"set_rc_pwm",
		_rpitx_set_rc_pwm,
		METH_VARARGS,
		"Sets the RC PWM parameters.\n\n"
			"Args:\n"
			"   frequency (float): Frequency to broadcast on.\n"
			"   dead_frequency (float): Frequency to broadcast on for gaps.\n"
			"   burst_us (int): Base time in nanosecnds.\n"
			"   synchronization_burst_count (int): Number of bursts for synchronization signal.\n"
			"   synchronization_multiple (int): Length of synchronization burst as multiple of burstUs.\n"
			"   burst_count (int): Number of bursts for command signal.\n"
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

	// Static initializers
	rcRunning = 0;
	rcInitialized = 0;

#if PY_MAJOR_VERSION >= 3
	return module;
#endif
}
