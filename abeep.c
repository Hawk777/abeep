/*
 * abeep - identical to "beep", except uses ALSA instead of the console, and
 * doesn't do the crazy stdin beep thing.
 *
 * Try abeep -h for command line args
 *
 * Parts of this code are copyright (C) Christopher Head, 2008.
 * Parts of this code are copyright (C) Johnathan Nightingale, 2000.
 *
 * This code may distributed only under the terms of the GNU Public License
 * which can be found at http://www.gnu.org/copyleft or in the file COPYING
 * supplied with this code.
 *
 * This code is not distributed with warranties of any kind, including implied
 * warranties of merchantability or fitness for a particular use or ability to
 * breed pandas in captivity, it just can't be done.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <getopt.h>
#include <alsa/asoundlib.h>
#include "sintable.h"

/* Meaningful Defaults */
#define DEFAULT_FREQ      440.0  /* Middle A */
#define DEFAULT_LENGTH    200    /* milliseconds */
#define DEFAULT_REPS      1
#define DEFAULT_DELAY     100    /* milliseconds */
#define DEFAULT_END_DELAY NO_END_DELAY

/* Other Constants */
#define NO_END_DELAY      0
#define YES_END_DELAY     1



typedef struct beep_parms_t {
	float freq;                   /* tone frequency (Hz) */
	int length;                   /* tone length (ms) */
	int reps;                     /* # of repetitions */
	int delay;                    /* delay between reps (ms) */
	int end_delay;                /* do we delay after last rep? */
	struct beep_parms_t *next;    /* in case -n/--new is used. */
} beep_parms_t;


static snd_pcm_t *pcm_handle = 0;
static int16_t *buffer = 0;
static snd_pcm_uframes_t buffer_size = 0;
static snd_pcm_uframes_t buffer_used = 0;
static unsigned int sample_rate = 44100;
static uint64_t nco_accumulator = 0;
static uint64_t last_fcw = 0;


/* print usage and exit */
static void usage_bail(const char *executable_name) {
	printf("Usage:\n%s [-f freq] [-l length] [-r reps] [-d delay] "
	       "[-D delay]\n",
	       executable_name);
	printf("%s [Options...] [-n] [--new] [Options...] ... \n", executable_name);
	printf("%s [-h] [--help]\n", executable_name);
	exit(EXIT_FAILURE);
}


static void init(void) {
	snd_pcm_hw_params_t *hwparams = 0;
	int direction;

	snd_pcm_hw_params_alloca(&hwparams);
	if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fputs("Error opening PCM device\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_any(pcm_handle, hwparams) < 0) {
		fputs("Cannot configure this PCM device.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_set_access(pcm_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
		fputs("Cannot set interleaved mode.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_set_format(pcm_handle, hwparams, SND_PCM_FORMAT_S16_LE) < 0) {
		fputs("Cannot set format.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_set_rate_near(pcm_handle, hwparams, &sample_rate, 0) < 0) {
		fputs("Cannot set sample rate.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_set_channels(pcm_handle, hwparams, 1) < 0) {
		fputs("Cannot set channel count.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params_set_periods(pcm_handle, hwparams, 4, 0) < 0) {
		fputs("Cannot set period count.\n", stderr);
		exit(EXIT_FAILURE);
	}
	direction = 0;
	if (snd_pcm_hw_params_set_period_size_last(pcm_handle, hwparams, &buffer_size, &direction) < 0) {
		fputs("Cannot set period size to maximum.\n", stderr);
		exit(EXIT_FAILURE);
	}
	if (snd_pcm_hw_params(pcm_handle, hwparams) < 0) {
		fputs("Error setting HW params.\n", stderr);
		exit(EXIT_FAILURE);
	}
	buffer = malloc(sizeof(int16_t) * buffer_size);
	if (!buffer) {
		fputs("Cannot allocate buffer.\n", stderr);
		exit(EXIT_FAILURE);
	}
}


/* Parse the command line.	argv should be untampered, as passed to main.
 * Beep parameters returned in result, subsequent parameters in argv will over-
 * ride previous ones.
 *
 * Currently valid parameters:
 *	"-f <frequency in Hz>"
 *	"-l <tone length in ms>"
 *	"-r <repetitions>"
 *	"-d <delay in ms>"
 *	"-D <delay in ms>" (similar to -d, but delay after last repetition as well)
 *	"-h/--help"
 *	"-v/-V/--version"
 *	"-n/--new"
 *
 * March 29, 2002 - Daniel Eisenbud points out that c should be int, not char,
 * for correctness on platforms with unsigned chars.
 */
static void parse_command_line(int argc, char **argv, beep_parms_t *result) {
	int c;

	struct option opt_list[4] = {
		{"help",    0, NULL, 'h'},
		{"new",     0, NULL, 'n'},
		{0,         0, 0,    0}};
	while((c = getopt_long(argc, argv, "f:l:r:d:D:hn", opt_list, NULL)) != EOF) {
		int argval = -1; /* handle parsed numbers for various arguments */
		float argfreq = -1;
		switch(c) {
		case 'f': /* freq */
			if(!sscanf(optarg, "%f", &argfreq) || (argfreq >= 20000 /* ack! */) || (argfreq < 1))
				usage_bail(argv[0]);
			else
				result->freq = argfreq;
			break;
		case 'l': /* length */
			if(!sscanf(optarg, "%d", &argval) || (argval <= 0))
				usage_bail(argv[0]);
			else
				result->length = argval;
			break;
		case 'r': /* repetitions */
			if(!sscanf(optarg, "%d", &argval) || (argval <= 0))
				usage_bail(argv[0]);
			else
				result->reps = argval;
			break;
		case 'd': /* delay between reps - WITHOUT delay after last beep*/
			if(!sscanf(optarg, "%d", &argval) || (argval < 0))
				usage_bail(argv[0]);
			else {
				result->delay = argval;
				result->end_delay = NO_END_DELAY;
			}
			break;
		case 'D': /* delay between reps - WITH delay after last beep */
			if(!sscanf(optarg, "%d", &argval) || (argval < 0))
				usage_bail(argv[0]);
			else {
				result->delay = argval;
				result->end_delay = YES_END_DELAY;
			}
			break;
		case 'n': /* also --new - create another beep */
			result->next = malloc(sizeof(beep_parms_t));
			result->next->freq       = DEFAULT_FREQ;
			result->next->length     = DEFAULT_LENGTH;
			result->next->reps       = DEFAULT_REPS;
			result->next->delay      = DEFAULT_DELAY;
			result->next->end_delay  = DEFAULT_END_DELAY;
			result->next->next       = NULL;
			result = result->next; /* yes, I meant to do that. */
			break;
		case 'h': /* notice that this is also --help */
		default:
			usage_bail(argv[0]);
		}
	}
}


static void send_buffer_to_card(void) {
	snd_pcm_sframes_t ret;

	while (buffer_used) {
		ret = snd_pcm_writei(pcm_handle, buffer, buffer_used);
		if (ret == -EPIPE) {
			fputs("WARNING: buffer underrun!\n", stderr);
			snd_pcm_prepare(pcm_handle);
			return;
		} else if (ret < 0) {
			fputs("Cannot send data to sound card!\n", stderr);
			exit(EXIT_FAILURE);
		}
		memmove(buffer, buffer + ret, (buffer_used - ret) * sizeof(int16_t));
		buffer_used -= ret;
	}
}


static void play_sample(int16_t sample) {
	while (buffer_used == buffer_size)
		send_buffer_to_card();
	buffer[buffer_used++] = sample;
}


static void play_fcw(uint64_t fcw, unsigned int samples) {
	unsigned int index;

	while (samples--) {
		nco_accumulator += fcw;
		/* accumulator is 64 bits, sintable is 256k, uses upper 18 bits for addressing, lower 46 for rounding */
		index = (nco_accumulator + UINT64_C(0x200000000000)) >> 46;
		play_sample(sintable(index));
	}

	last_fcw = fcw;
}


static void play_silence(unsigned int samples) {
	unsigned int index;

	/* eliminate DC offset by ramping sample level towards zero */
	while (samples--) {
		nco_accumulator += last_fcw;
		/* accumulator is 64 bits, sintable is 256k, uses upper 18 bits for addressing, lower 46 for rounding */
		index = (nco_accumulator + UINT64_C(0x200000000000)) >> 46;
		if (abs(sintable(index)) < 1000) {
			++samples;
			break;
		}
		play_sample(sintable(index));
	}

	if (samples) {
		/* now just play the rest of the samples as plain silence */
		nco_accumulator = 0;
		while (samples--)
			play_sample(0);
	}

	last_fcw = 0;
}


static void play_frequency(double frequency, unsigned int samples) {
	uint64_t fcw = frequency * SINTABLE_SIZE / sample_rate * UINT64_C(0x400000000000);

	if (frequency > 2)
		play_fcw(fcw, samples);
	else
		play_silence(samples);
}


static void play_blocks(const beep_parms_t *parms) {
	int i;

	while (parms) {
		for (i = 0; i < parms->reps; i++) {
			/* play the tone for the requisite amount of time. */
			play_frequency(parms->freq, (parms->length * sample_rate + 500) / 1000);

			/* play silence for the requisite amount of time, IF this is not the last rep or if we have an end delay. */
			if (i + 1 < parms->reps || parms->end_delay)
				play_frequency(0, (parms->delay * sample_rate + 500) / 1000);
		}
		parms = parms->next;
	}
}


static void cleanup(void) {
	while (buffer_used)
		send_buffer_to_card();
	snd_pcm_drain(pcm_handle);
	snd_pcm_close(pcm_handle);
}


int main(int argc, char **argv) {
	beep_parms_t *parms = malloc(sizeof(beep_parms_t));
	parms->freq       = DEFAULT_FREQ;
	parms->length     = DEFAULT_LENGTH;
	parms->reps       = DEFAULT_REPS;
	parms->delay      = DEFAULT_DELAY;
	parms->end_delay  = DEFAULT_END_DELAY;
	parms->next       = NULL;

	init();

	parse_command_line(argc, argv, parms);
	play_blocks(parms);

	cleanup();

	return EXIT_SUCCESS;
}

