/*
 * madplay - MPEG audio decoder and player
 * Copyright (C) 2000-2004 Robert Leslie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 */

# ifdef HAVE_CONFIG_H
#  include "config.h"
# endif

# include "global.h"

# include <unistd.h>
# include <errno.h>
# include <sjpcm.h>
# include <string.h>
# include <mad.h>

# include "gettext.h"

# include "audio.h"

# if defined(WORDS_BIGENDIAN)
#  define audio_pcm_s16  audio_pcm_s16be
# else
#  define audio_pcm_s16  audio_pcm_s16le
# endif

#include <sifrpc.h>
#include <sifcmd.h>
#include <loadfile.h>

static char const *host;

int loadModules()
{
	int ret = 0;

	ret |= SifLoadModule("rom0:LIBSD", 0, NULL);
	ret |= SifLoadModule("host:irx/sjpcm.irx", 0, NULL); 
	
	return ret;
}

int vsync_num __attribute__((aligned (16)));
int frame_num __attribute__((aligned (16)));
int current_buffer __attribute__((aligned (16)));
int buffered __attribute__((aligned (16)));

// taken from gslib
void EnableVSyncCallbacks(void)
{
	asm __volatile__ ("
		di

		addiu $4, $0, 2	 
		addiu $3, $0, 20

		syscall
		nop

		ei
	");
}

// taken from gslib
unsigned int AddVSyncCallback(void (*func_ptr)())
{
	unsigned int AddCallbackID;

	asm __volatile__ ("
		di

		# 'func_ptr' param will have been passed in $4, so move it to $5 (needed for syscall)

		addu  $5, $0, %1
		addiu $4, $0, 2		# 2 = vsync_start	

		addiu $6, $0, 0
		addiu $3, $0, 16	# AddIntcHandler

		syscall				# Returns assigned ID in $2
		nop

		addu  %0, $0, $2
		
		addiu $4, $0, 2	 
		addiu $3, $0, 20

		syscall
		nop

		ei"
	: "=r" (AddCallbackID)
	: "g" (func_ptr)
	);

//		la $4, AddCallbackID	# Store ID in var
//		sw $2, 0($4)


	// Enable VSync callbacks if not already enabled
	EnableVSyncCallbacks();

	return AddCallbackID;
}

#define FRAME_SIZE	48000 // 1 frame
#define FRAMES	50 // 60 for ntsc
#define TICK	(FRAME_SIZE / FRAMES) 	

// store 2 channels 
unsigned short hold[2][FRAME_SIZE] __attribute__((aligned (16)));
unsigned int held __attribute__((aligned (16))); // number of samples buffd 
// ... for incomplete tick

unsigned int vsync_func(void)
{

// doh! cant do this here!
//	SjPCM_Enqueue(...);

	vsync_num++;
	asm __volatile__ ("ei");

  return 0;
}

typedef void (*functionPointer)();

static
int init(struct audio_init *init)
{
  host = init->path;
  if (host && *host == 0)
    host = 0;

	/* load modules... maybe shouldnt do this here */
  SifInitRpc(0);
  if (loadModules() < 0) {
		printf ("Failed to load modules\n");
		return -1;
  }

  /* sound */
  SjPCM_Init(1);
 	SjPCM_Clearbuff();
  SjPCM_Setvol(0x3fff);
  SjPCM_Play();

	/* buffer */
	bzero(hold, sizeof(hold));
	held = 0;
	buffered = 0;
	current_buffer = 0;

	/* vsync callback */
	vsync_num=0;
	frame_num=0;
	AddVSyncCallback((functionPointer)&vsync_func);

  return 0;

}

static
int config(struct audio_config *config)
{
  config->channels  = 2;
  config->speed     = 48000;
  config->precision = 16;
  return 0;
}

#define csr			0x12001000	// System status and reset
#define CSR			((volatile u64 *)(csr))

inline void wait_vsync(void)
{
	*CSR = *CSR & 8;
	while(!(*CSR & 8));
}

static struct audio_dither left_dither, right_dither;

unsigned int audio_pcm_sjpcm(unsigned short *leftout, unsigned short *rightout, unsigned int nsamples,
			  mad_fixed_t const *left, mad_fixed_t const *right,
			  enum audio_mode mode, struct audio_stats *stats)
{
  unsigned int len;

  len = nsamples;

  if (right) {  /* stereo */
    switch (mode) {
    case AUDIO_MODE_ROUND:
      while (len--) {
	leftout[0] = audio_linear_round(16, *left++,  stats);
	rightout[0] = audio_linear_round(16, *right++, stats);

	leftout++;
	rightout++;
      }
      break;

    case AUDIO_MODE_DITHER:
      while (len--) {
	leftout[0] = audio_linear_dither(16, *left++, &left_dither, stats);
	rightout[0] = audio_linear_dither(16, *right++, &right_dither, stats);

	leftout++;
	rightout++;
      }
      break;

    default:
      return 0;
    }

    return nsamples;
  }
  else {  /* mono */
    switch (mode) {
    case AUDIO_MODE_ROUND:
      while (len--)
	*leftout++ = audio_linear_round(16, *left++, stats);
      break;

    case AUDIO_MODE_DITHER:
      while (len--)
	*leftout++ = audio_linear_dither(16, *left++, &left_dither, stats);
      break;

    default:
      return 0;
    }

    return nsamples;
  }
}

static
void output() 
{
	int offset;

//	printf ("output\n");

	offset = (current_buffer % FRAMES) * TICK;

	if ((frame_num < vsync_num) && (buffered - current_buffer)) { 
		SjPCM_Enqueue(&(hold[0][offset]), &(hold[1][offset]), TICK, 0); 
		frame_num = vsync_num;
		current_buffer++;
	}
}

static
int buffer(unsigned short const *leftptr, unsigned short const *rightptr, signed int len)
{
  unsigned int grab;

// printf ("buffer(%p, %d)\n", ptr, len);

	while ((buffered - current_buffer) > 10) // prebuffer amount
	{
		// buffer already has something
		output();
	}

	while (len > 0) {

		// grab a ticks worth of data
    	grab = TICK;

		// or grab enough to complete a tick
    	grab = (grab - held) < grab ? (grab - held) : grab; 

		// but dont grab too much
    	grab = len < grab ? len : grab; 

    	len -= grab;

//    	printf ("grab %5d held %5d buff %5d len %5d\n ", grab, held, buffered, len);

		// copy the data to the buffer
    	memcpy(&hold[0][(buffered % FRAMES) * TICK + held], leftptr, grab * 2);
    	memcpy(&hold[1][(buffered % FRAMES) * TICK + held], rightptr, grab * 2);

		if ((grab + held) == TICK) {

			// a complete tick has been buffered
			buffered++;
			held = 0;
    		leftptr  += grab;
    		rightptr  += grab;

		} else {

			// only part of a tick stored... remember for next iteration
			held += grab; 

		}

  }

  return 0;
}

static
int play(struct audio_play *play)
{
  unsigned short left[MAX_NSAMPLES];
  unsigned short right[MAX_NSAMPLES];
  signed int len;

// printf ("play %d\n", play->nsamples);

  len = audio_pcm_sjpcm(left, right, play->nsamples,
			play->samples[0], play->samples[1],
			play->mode, play->stats);

//  if (frame_num == 0) 
//		frame_num = vsync_num; // catch up 

//	printf ("%5d %5d %5d %5d %d\n", buffered, current_buffer, vsync_num, frame_num, len);

  return buffer(left, right, len);
}

static
int stop(struct audio_stop *stop)
{
  SjPCM_Pause();

  return 0;
}

void DisableVSyncCallbacks(void)
{
	asm __volatile__ ("
		di

		addiu $4, $0, 2	 
		addiu $3, $0, 21

		syscall
		nop


		ei
	");
}

static
int finish(struct audio_finish *finish)
{
  SjPCM_Pause();
	DisableVSyncCallbacks();

  return 0;
}

int audio_sjpcm(union audio_control *control)
{
  audio_error = 0;

//  printf ("audio_sjpcm()\n");

  switch (control->command) {
  case AUDIO_COMMAND_INIT:
//printf ("AUDIO_COMMAND_INIT\n");
    return init(&control->init);

  case AUDIO_COMMAND_CONFIG:
//printf ("AUDIO_COMMAND_CONFIG\n");
    return config(&control->config);

  case AUDIO_COMMAND_PLAY:
//printf ("AUDIO_COMMAND_PLAY\n");
    return play(&control->play);

  case AUDIO_COMMAND_STOP:
//printf ("AUDIO_COMMAND_STOP\n");
    return stop(&control->stop);

  case AUDIO_COMMAND_FINISH:
//printf ("AUDIO_COMMAND_FINISH\n");
    return finish(&control->finish);
  }

  return 0;
}
