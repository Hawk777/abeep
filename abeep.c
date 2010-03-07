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

#define PERIODS_PER_BLOCK 20 /* number of periods in a single tone's block */



typedef struct beep_parms_t {
	float freq;                   /* tone frequency (Hz) */
	int length;                   /* tone length (ms) */
	int reps;                     /* # of repetitions */
	int delay;                    /* delay between reps (ms) */
	int end_delay;                /* do we delay after last rep? */
	int16_t *blk;                 /* a block of samples */
	snd_pcm_uframes_t blk_frames; /* the number of frames in "blk" */
	struct beep_parms_t *next;    /* in case -n/--new is used. */
} beep_parms_t;


static snd_pcm_t *pcm_handle = 0;
static int16_t *buffer = 0;
static snd_pcm_uframes_t buffer_size;
static snd_pcm_uframes_t buffer_used = 0;
static unsigned int sample_rate = 44100;


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


static void build_blocks(beep_parms_t *parms) {
	snd_pcm_uframes_t i;
	double fraction_of_period;

	while (parms) {
		parms->blk_frames = (snd_pcm_uframes_t) (1.0 / parms->freq * sample_rate * PERIODS_PER_BLOCK);
		parms->blk = malloc(sizeof(int16_t) * parms->blk_frames);
		for (i = 0; i < parms->blk_frames; i++) {
			fraction_of_period = ((double) i) / (parms->blk_frames - 1) * PERIODS_PER_BLOCK;
			fraction_of_period -= floor(fraction_of_period);
			if (fraction_of_period < 0.25)
				parms->blk[i] =  SINTABLE[(unsigned int) (fraction_of_period         * 4 * (SINTABLE_SIZE - 1))];
			else if (fraction_of_period < 0.5)
				parms->blk[i] =  SINTABLE[(unsigned int) ((0.5 - fraction_of_period) * 4 * (SINTABLE_SIZE - 1))];
			else if (fraction_of_period < 0.75)
				parms->blk[i] = -SINTABLE[(unsigned int) ((fraction_of_period - 0.5) * 4 * (SINTABLE_SIZE - 1))];
			else
				parms->blk[i] = -SINTABLE[(unsigned int) ((1.0 - fraction_of_period) * 4 * (SINTABLE_SIZE - 1))];
		}
		parms = parms->next;
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


static void play_buffered(const void *data, snd_pcm_uframes_t length) {
	const int16_t *ptr = data;
	snd_pcm_uframes_t buffer_avail, tocopy;

	while (length) {
		while (buffer_used == buffer_size)
			send_buffer_to_card();
		buffer_avail = buffer_size - buffer_used;
		tocopy = length < buffer_avail ? length : buffer_avail;
		memcpy(buffer + buffer_used, ptr, tocopy * sizeof(int16_t));
		buffer_used += tocopy;
		ptr += tocopy;
		length -= tocopy;
	}
}


static void play_silence(snd_pcm_uframes_t length) {
	snd_pcm_uframes_t buffer_avail, tofill;

	while (length) {
		while (buffer_used == buffer_size)
			send_buffer_to_card();
		buffer_avail = buffer_size - buffer_used;
		tofill = length < buffer_avail ? length : buffer_avail;
		memset(buffer + buffer_used, 0, tofill * sizeof(int16_t));
		buffer_used += tofill;
		length -= tofill;
	}
}


static void play_blocks(const beep_parms_t *parms) {
	snd_pcm_uframes_t duration_in_frames;
	unsigned int i;

	while (parms) {
		for (i = 0; i < parms->reps; i++) {
			/* play the tone for the requisite amount of time. */
			duration_in_frames = (parms->length * sample_rate + 500) / 1000;
			while (duration_in_frames > parms->blk_frames) {
				play_buffered(parms->blk, parms->blk_frames);
				duration_in_frames -= parms->blk_frames;
			}
			play_buffered(parms->blk, duration_in_frames);

			/* play silence for the requisite amount of time, IF this is not the last rep or if we have an end delay. */
			if (i < parms->reps - 1 || parms->end_delay)
				play_silence((parms->delay * sample_rate + 500) / 1000);
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
	build_blocks(parms);
	play_blocks(parms);

	cleanup();

	return EXIT_SUCCESS;
}

