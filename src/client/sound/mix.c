/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "sound.h"

#define    PAINTBUFFER_SIZE    2048

static int snd_scaletable[32][256];
static int snd_vol;

static void TransferStereo16(samplepair_t *samp, int endtime)
{
    for (int ltime = paintedtime; ltime < endtime;) {
        // handle recirculating buffer issues
        int lpos = ltime & ((dma.samples >> 1) - 1);
        int count = (dma.samples >> 1) - lpos;
        if (ltime + count > endtime)
            count = endtime - ltime;

        // write a linear blast of samples
        int16_t *out = (int16_t *)dma.buffer + (lpos << 1);
        for (int i = 0; i < count; i++, samp++, out += 2) {
            int left = samp->left >> 8;
            int right = samp->right >> 8;
            out[0] = clamp(left, INT16_MIN, INT16_MAX);
            out[1] = clamp(right, INT16_MIN, INT16_MAX);
        }

        ltime += count;
    }
}

static void TransferStereo(samplepair_t *samp, int endtime)
{
    int *p = (int *)samp;
    int count = (endtime - paintedtime) * dma.channels;
    int out_mask = dma.samples - 1;
    int out_idx = paintedtime * dma.channels & out_mask;
    int step = 3 - dma.channels;
    int val;

    if (dma.samplebits == 16) {
        int16_t *out = (int16_t *)dma.buffer;
        while (count--) {
            val = *p >> 8;
            p += step;
            clamp(val, INT16_MIN, INT16_MAX);
            out[out_idx] = val;
            out_idx = (out_idx + 1) & out_mask;
        }
    } else if (dma.samplebits == 8) {
        uint8_t *out = (uint8_t *)dma.buffer;
        while (count--) {
            val = *p >> 8;
            p += step;
            clamp(val, INT16_MIN, INT16_MAX);
            out[out_idx] = (val >> 8) + 128;
            out_idx = (out_idx + 1) & out_mask;
        }
    }
}

static void TransferPaintBuffer(samplepair_t *samp, int endtime)
{
    if (s_testsound->integer) {
        int i;

        // write a fixed sine wave
        for (i = paintedtime; i < endtime; i++) {
            samp[i].left = samp[i].right = sin(i * 0.1f) * 20000 * 256;
        }
    }

    if (dma.samplebits == 16 && dma.channels == 2) {
        // optimized case
        TransferStereo16(samp, endtime);
    } else {
        // general case
        TransferStereo(samp, endtime);
    }
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

static void Paint8(channel_t *ch, sfxcache_t *sc, int count, samplepair_t *samp)
{
    int data;
    int *lscale, *rscale;
    uint8_t *sfx;
    int i;

    if (ch->leftvol > 255)
        ch->leftvol = 255;
    if (ch->rightvol > 255)
        ch->rightvol = 255;

    lscale = snd_scaletable[ch->leftvol >> 3];
    rscale = snd_scaletable[ch->rightvol >> 3];
    sfx = (uint8_t *)sc->data + ch->pos;

    for (i = 0; i < count; i++, samp++) {
        data = *sfx++;
        samp->left += lscale[data];
        samp->right += rscale[data];
    }

    ch->pos += count;
}

static void Paint16(channel_t *ch, sfxcache_t *sc, int count, samplepair_t *samp)
{
    int data;
    int left, right;
    int leftvol, rightvol;
    int16_t *sfx;
    int i;

    leftvol = ch->leftvol * snd_vol;
    rightvol = ch->rightvol * snd_vol;
    sfx = (int16_t *)sc->data + ch->pos;

    for (i = 0; i < count; i++, samp++) {
        data = *sfx++;
        left = (data * leftvol) >> 8;
        right = (data * rightvol) >> 8;
        samp->left += left;
        samp->right += right;
    }

    ch->pos += count;
}

void S_PaintChannels(int endtime)
{
    samplepair_t paintbuffer[PAINTBUFFER_SIZE];
    int i;
    int end;
    channel_t *ch;
    sfxcache_t *sc;
    int ltime, count;
    playsound_t *ps;

    while (paintedtime < endtime) {
        // if paintbuffer is smaller than DMA buffer
        end = endtime;
        if (end - paintedtime > PAINTBUFFER_SIZE)
            end = paintedtime + PAINTBUFFER_SIZE;

        // start any playsounds
        while (1) {
            ps = s_pendingplays.next;
            if (ps == &s_pendingplays)
                break;    // no more pending sounds
            if (ps->begin <= paintedtime) {
                S_IssuePlaysound(ps);
                continue;
            }

            if (ps->begin < end)
                end = ps->begin;        // stop here
            break;
        }

        // clear the paint buffer
        memset(paintbuffer, 0, (end - paintedtime) * sizeof(samplepair_t));

        // paint in the channels.
        ch = channels;
        for (i = 0; i < s_numchannels; i++, ch++) {
            ltime = paintedtime;

            while (ltime < end) {
                if (!ch->sfx || (!ch->leftvol && !ch->rightvol))
                    break;

                // max painting is to the end of the buffer
                count = end - ltime;

                // might be stopped by running out of data
                if (ch->end - ltime < count)
                    count = ch->end - ltime;

                sc = S_LoadSound(ch->sfx);
                if (!sc)
                    break;

                if (count > 0 && ch->sfx) {
                    samplepair_t *samp = &paintbuffer[ltime - paintedtime];
                    if (sc->width == 1)
                        Paint8(ch, sc, count, samp);
                    else
                        Paint16(ch, sc, count, samp);

                    ltime += count;
                }

                // if at end of loop, restart
                if (ltime >= ch->end) {
                    if (ch->autosound) {
                        // autolooping sounds always go back to start
                        ch->pos = 0;
                        ch->end = ltime + sc->length;
                    } else if (sc->loopstart >= 0) {
                        ch->pos = sc->loopstart;
                        ch->end = ltime + sc->length - ch->pos;
                    } else {
                        // channel just stopped
                        ch->sfx = NULL;
                    }
                }
            }

        }

        // transfer out according to DMA format
        TransferPaintBuffer(paintbuffer, end);
        paintedtime = end;
    }
}

void S_InitScaletable(void)
{
    int        i, j;
    int        scale;

    Cvar_ClampValue(s_volume, 0, 1);

    snd_vol = s_volume->value * 256;
    for (i = 0; i < 32; i++) {
        scale = i * 8 * snd_vol;
        for (j = 0; j < 256; j++) {
            snd_scaletable[i][j] = (j - 128) * scale;
        }
    }

    s_volume->modified = false;
}
