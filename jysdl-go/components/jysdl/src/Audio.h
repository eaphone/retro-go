/* Adapted from jysdl-bh (scarsty) Audio.h for retro-go.
 *
 * The original used BASS or SDL3_mixer + TinySoundFont. On ESP32 we target
 * retro-go's rg_audio. Audio playback is stubbed for now and will be wired to
 * rg_audio + TinySoundFont (tsf.h is bundled) in a follow-up.
 */
#pragma once
#include <string>

#define WAVNUM 5

class Audio
{
public:
    static Audio* getInstance()
    {
        static Audio a;
        return &a;
    }

    void init();
    void quit();
    int PlayMIDI(const char* filename);
    int StopMIDI();
    int PausedMIDI();
    int ResumeMIDI();
    int PlayWAV(const char* filename);
    void SetVolume(int volume);

private:
    Audio() = default;
    ~Audio() = default;

    int currentWav = 0;
    static char currentfile[255];
};
