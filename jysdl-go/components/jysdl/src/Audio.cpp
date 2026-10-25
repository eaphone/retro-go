/* Adapted from jysdl-bh (scarsty) Audio.cpp for retro-go.
 *
 * Audio is stubbed for now. A real implementation will use rg_audio_submit()
 * with TinySoundFont (tsf.h / tml.h, bundled) for MIDI and a simple WAV mixer
 * for sound effects.
 */
#include "Audio.h"
#include "jymain.h"

char Audio::currentfile[255] = "";

void Audio::init()
{
    (void)currentWav;
}

void Audio::quit()
{
}

int Audio::PlayMIDI(const char* filename)
{
    (void)filename;
    return 0;
}

int Audio::StopMIDI()
{
    return 0;
}

int Audio::PausedMIDI()
{
    return 0;
}

int Audio::ResumeMIDI()
{
    return 0;
}

int Audio::PlayWAV(const char* filename)
{
    (void)filename;
    return 0;
}

void Audio::SetVolume(int volume)
{
    (void)volume;
}
